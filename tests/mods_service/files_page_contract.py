"""File index service/cache contracts and a synthetic load-check cost measurement; no game data."""
import json
import ctypes
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import mods_service_contract as contract

SERVICE, FAKE, HELPER = map(Path, sys.argv[1:4])
contract.SERVICE = SERVICE


class FilesPageContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="eawr-files-page-")
        self.root = Path(self.temporary.name)
        self.game, self.user, self.mod = (self.root / name for name in ("game", "user", "mod"))
        for path in (self.game / "corruption/Data", self.game / "GameData/Data", self.mod / "Data"):
            path.mkdir(parents=True)
            (path / "marker").write_text("synthetic")
        (self.game / "corruption/StarWarsG.exe").write_bytes(b"")
        self.client = contract.Interactive(args=["--user-data", str(self.user), "--loadcheck-helper", str(FAKE)])
        self.counter = 0
        hello = self.call("hello")
        self.assertEqual(hello["result"]["protocol"], {"major": 1, "minor": 6})
        self.assertIn("files.page", hello["result"]["capabilities"])
        self.assertIn("result", self.call("game.find", hint=str(self.game)))
        self.profile = {"id": "file-index", "name": "file-index", "layers": [{"key": "mods:Leaf", "root": str(self.mod)}]}
        self.assertIn("result", self.call("profile.save", profile=self.profile))

    def tearDown(self):
        self.client.close()
        self.client.wait()
        for path in self.root.rglob("*"):
            try:
                path.chmod(0o700 if path.is_dir() else 0o600)
            except OSError:
                pass
        self.temporary.cleanup()

    def call(self, method, **params):
        self.counter += 1
        self.client.send(contract.hello(self.counter, min_minor=1) if method == "hello"
                         else contract.request(self.counter, method, **params))
        while True:
            response = self.client.receive()
            if "event" not in response:
                self.assertEqual(response.get("id"), self.counter)
                return response

    def build(self):
        response = self.call("loadcheck.start", profile=self.profile["id"])
        self.assertTrue(response["result"]["report"]["ok"], response)

    def page(self, **params):
        return self.call("files.page", profile=self.profile["id"], **params)

    def test_ready_paging_filters_and_flags(self):
        missing = self.page()["result"]
        self.assertFalse(missing["ready"])
        self.assertTrue(missing["requires_loadcheck"])
        self.build()
        first = self.page(limit=1)["result"]
        self.assertTrue(first["ready"])
        self.assertEqual(first["index_version"], 1)
        self.assertEqual(first["total"], 3)
        self.assertEqual(len(first["chain_fingerprint"]), 64)
        self.assertIsNone(first["rows"][0]["winner"]["sha256"])
        self.assertEqual(first["rows"][0]["winner"]["role"], "only")
        second = self.page(cursor=first["next_cursor"], limit=1)["result"]
        self.assertEqual(second["rows"][0]["path"], "data/xml/changed.xml")
        self.assertEqual(second["rows"][0]["candidates"][1]["role"], "lose")
        third = self.page(cursor=second["next_cursor"], limit=500)["result"]
        self.assertTrue(third["rows"][0]["identical"])
        self.assertIsNone(third["next_cursor"])
        filtered = self.page(filters={"layer": "expansion", "role": "loser", "type": "XML", "path": "CHANGED",
                                      "conflicts_only": True, "hide_identical": True})["result"]
        self.assertEqual(filtered["total"], 1)
        self.assertEqual(filtered["rows"][0]["path"], "data/xml/changed.xml")
        self.assertEqual(self.page(filters={"conflicts_only": True})["result"]["total"], 2)
        self.assertEqual(self.page(filters={"hide_identical": True})["result"]["total"], 2)
        self.assertEqual(self.page(filters={"layer": "missing"})["result"]["total"], 0)
        layers = {layer["key"]: layer for layer in first["layers"]}
        self.assertEqual(layers["mods:Leaf"], {"key": "mods:Leaf", "files": 3, "wins": 2, "loses": 0, "only": 1})
        self.assertEqual(layers["base"]["loses"], 1)
        stale = self.page(cursor=first["next_cursor"], filters={"path": "changed"})
        self.assertEqual(stale["error"]["code"], "EAWR-MODS-SVC-0005")
        self.assertLessEqual(len(contract.wire(first).encode("utf8")), contract.LIMIT)

    def test_invalid_inputs_versions_truncation_and_chain_changes(self):
        for params in ({"limit": 0}, {"limit": 501}, {"cursor": 5}, {"filters": []},
                       {"filters": {"role": "invented"}}, {"filters": {"hide_identical": 1}},
                       {"filters": {"layer": 1}}, {"cursor": "missing"}):
            with self.subTest(params=params):
                self.assertEqual(self.page(**params)["error"]["code"], "EAWR-MODS-SVC-0005")
        self.build()
        first = self.page(limit=1)["result"]
        cache = next((self.user / "mods/cache/loadcheck").glob("*.files"))
        data = cache.read_bytes()
        wrong = bytearray(data)
        struct.pack_into("<Q", wrong, 11, 2)
        cache.write_bytes(wrong)
        self.assertFalse(self.page()["result"]["ready"], "new cache versions require rebuild")
        self.assertEqual(self.page(cursor=first["next_cursor"])["error"]["code"], "EAWR-MODS-SVC-0005")
        cache.write_bytes(data[:-1])
        self.assertFalse(self.page()["result"]["ready"], "truncated cache is unavailable")
        self.build()
        first = self.page(limit=1)["result"]
        (self.mod / "Data/marker").write_text("changed installed bytes and size")
        changed = self.page()["result"]
        self.assertFalse(changed["ready"])
        self.assertNotEqual(changed["chain_fingerprint"], first["chain_fingerprint"])
        self.assertEqual(self.page(cursor=first["next_cursor"])["error"]["code"], "EAWR-MODS-SVC-0005")

    def test_read_only_roots_and_wrong_profile_cursor(self):
        roots = (self.mod, self.game)
        before = {str(path.relative_to(self.root)): path.read_bytes() for root in roots for path in root.rglob("*") if path.is_file()}
        for root in roots:
            for path in root.rglob("*"):
                path.chmod(0o555 if path.is_dir() else 0o444)
        self.build()
        first = self.page(limit=1)["result"]
        other = dict(self.profile, id="other", name="other")
        self.assertIn("result", self.call("profile.save", profile=other))
        wrong = self.call("files.page", profile="other", cursor=first["next_cursor"])
        self.assertEqual(wrong["error"]["code"], "EAWR-MODS-SVC-0005")
        after = {str(path.relative_to(self.root)): path.read_bytes() for root in roots for path in root.rglob("*") if path.is_file()}
        self.assertEqual(before, after)

    def test_index_survives_service_restart(self):
        self.build()
        expected = self.page()["result"]
        self.client.close()
        self.assertEqual(self.client.wait(), 0)
        self.client = contract.Interactive(args=["--user-data", str(self.user), "--loadcheck-helper", str(FAKE)])
        self.call("hello")
        self.call("game.find", hint=str(self.game))
        self.assertEqual(self.page()["result"], expected)

    def test_cancel_does_not_publish_partial_index(self):
        self.profile = dict(self.profile, id="file-index-slow", name="file-index-slow")
        self.assertIn("result", self.call("profile.save", profile=self.profile))
        self.counter += 1
        start = self.counter
        self.client.send(contract.request(start, "loadcheck.start", profile=self.profile["id"]))
        self.assertEqual(self.client.receive()["event"], "loadcheck.progress")
        self.counter += 1
        self.client.send(contract.request(self.counter, "loadcheck.cancel", request=start))
        answers = {answer["id"]: answer for answer in (self.client.receive(), self.client.receive())}
        self.assertEqual(answers[start]["error"]["code"], "EAWR-MODS-SVC-0009")
        self.assertTrue(answers[self.counter]["result"]["cancelled"])
        self.assertFalse(self.page()["result"]["ready"])
        self.assertFalse(list((self.user / "mods/cache/loadcheck").glob("running-*")))

    def test_partial_index_does_not_fail_service_load_check(self):
        self.build()
        self.assertTrue(self.page()["result"]["ready"])
        self.profile = dict(self.profile, name="file-index-partial-index")
        self.assertIn("result", self.call("profile.save", profile=self.profile))
        response = self.call("loadcheck.start", profile=self.profile["id"])
        report = response["result"]["report"]
        self.assertTrue(report["ok"], response)
        self.assertEqual(report["stage"], "complete")
        self.assertEqual(report["warning_count"], 1)
        self.assertEqual(report["diagnostics"][0]["code"], "EAWR-MODS-SVC-0013")
        self.assertFalse(self.page()["result"]["ready"])
        self.assertFalse(list((self.user / "mods/cache/loadcheck").glob("*.files")))
        self.assertFalse(list((self.user / "mods/cache/loadcheck").glob("running-*")))

    def real_helper_profile(self):
        for path in (self.game / "corruption/Data", self.game / "GameData/Data"):
            (path / "MegaFiles.xml").write_text("<Mega_Files><File>fixture.meg</File></Mega_Files>")
            name, payload = b"Data/XML/fixture.xml", b"<Fixture/>"
            archive = struct.pack("<IIH", 1, 1, len(name)) + name
            archive += struct.pack("<IIIII", 0x12345678, 0, len(payload), 8 + 2 + len(name) + 20, 0) + payload
            (path / "fixture.meg").write_bytes(archive)
        profile = self.root / "benchmark-profile.json"
        profile.write_text(json.dumps(dict(self.profile, format="eawr-mod-profile", version=1)))
        self.user.mkdir(exist_ok=True)
        return profile

    def real_helper(self, profile, index=None):
        command = [str(HELPER), str(profile), str(self.user), str(self.user / "benchmark-report.json"), str(self.game)]
        if index is not None:
            command += ["a" * 64, str(index)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout.splitlines()[-1])["report"]

    def test_real_helper_index_failures_preserve_load_check(self):
        profile = self.real_helper_profile()
        art = self.mod / "Data/shared.bin"
        art.write_bytes(b"same")
        (self.game / "corruption/Data/shared.bin").write_bytes(b"same")
        baseline = self.real_helper(profile)
        self.assertEqual(baseline["stage"], "catalog", baseline)
        index = self.user / "failed.files"
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        # Enumeration succeeds, but the optional conflicting art candidate cannot be read.
        handle = kernel.CreateFileW(str(art), 0x80000000, 0, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            unreadable = self.real_helper(profile, index)
        finally:
            kernel.CloseHandle(handle)
        # A missing output parent exercises the real writer's stream-failure path.
        unwritable = self.real_helper(profile, self.user / "missing/cache.files")
        for report in (unreadable, unwritable):
            self.assertEqual(report["stage"], baseline["stage"])
            self.assertEqual(report["ok"], baseline["ok"])
            self.assertEqual(report["error_count"], baseline["error_count"])
            self.assertEqual(report["warning_count"], baseline["warning_count"] + 1)
            warning = next(row for row in report["diagnostics"] if row["code"] == "EAWR-MODS-SVC-0013")
            self.assertEqual(warning["severity"], "warning")
        self.assertFalse(index.exists())
        self.assertFalse((self.user / "missing/cache.files").exists())

    def test_real_helper_many_file_load_check_delta(self):
        # Both invocations use the same normal helper/load-check pass. Its optional index
        # output is the only difference. The synthetic catalog intentionally has no units.
        data = self.mod / "Data"
        for i in range(2000):
            (data / f"unique-{i:05}.bin").write_bytes(b"u" * 1024)
        (data / "shared.bin").write_bytes(b"same")
        (self.game / "corruption/Data/shared.bin").write_bytes(b"same")
        profile = self.real_helper_profile()
        index = self.user / "benchmark.files"
        samples = {"baseline": [], "indexed": []}
        for mode in ("baseline", "indexed"):
            for _ in range(3):
                begin = time.perf_counter()
                report = self.real_helper(profile, index if mode == "indexed" else None)
                samples[mode].append(time.perf_counter() - begin)
                self.assertEqual(report["stage"], "catalog", report)
        self.assertTrue(index.is_file(), "real helper wrote an index before the synthetic catalog error")
        baseline = statistics.median(samples["baseline"])
        indexed = statistics.median(samples["indexed"])
        print("FILE-INDEX-BENCH " + json.dumps({"unique_files": 2000, "baseline_seconds": baseline,
              "indexed_seconds": indexed, "delta_seconds": indexed - baseline, "samples": samples}), flush=True)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
