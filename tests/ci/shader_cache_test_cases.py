"""Compatibility, bounded disk use and profile isolation for graphical test caches."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/rig"))
from shader_cache import ShaderCachePool, fingerprint  # noqa: E402
from rig_suite_runner import PinnedGodotEvidence  # noqa: E402

ADAPTERS = [{"Name": "Test GPU", "DriverVersion": "1", "PNPDeviceID": "device-1"}]


class ShaderCacheTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project = self.root / "project"
        (self.project / "bin").mkdir(parents=True)
        (self.project / "project.godot").write_text('[application]\nconfig/name="EAWR Viewer"\n')
        self.dll = self.project / "bin/viewer.dll"
        self.dll.write_bytes(b"viewer-one")
        self.identity = fingerprint(Path(sys.executable), self.project, ADAPTERS, [])
        self.pool = ShaderCachePool(self.root / "pool", size_cap=40)

    def files(self, user, shader=b"shader", pipeline=b"pipeline"):
        (user / "shader_cache/type").mkdir(parents=True)
        (user / "shader_cache/type/a.cache").write_bytes(shader)
        (user / "vulkan").mkdir()
        (user / "vulkan/pipelines.device.driver.cache").write_bytes(pipeline)
        (user / "vulkan/fault.json").write_text("private")
        (user / "logs").mkdir()
        (user / "logs/godot.log").write_text("private")
        (user / "fault.eawr-replay").write_text("private")

    def test_only_shader_and_pipeline_files_cross_profiles(self):
        first, second = self.root / "first", self.root / "second"
        self.files(first)
        self.assertEqual(self.pool.publish(self.identity, first), 14)
        self.assertEqual(self.pool.seed(self.identity, second), 14)
        self.assertEqual({str(p.relative_to(second)).replace('\\', '/') for p in second.rglob('*') if p.is_file()},
                         {"shader_cache/type/a.cache", "vulkan/pipelines.device.driver.cache"})

    def test_deep_shader_path_is_rejected_with_explicit_status(self):
        relative = Path("shader_cache/BestFitNormalShaderRD") / ("a" * 64) / ("b" * 40 + ".vulkan.cache")
        # 248 is below native MAX_PATH, but deliberately exceeds our 240 limit.
        first = self.root / ("p" * (248 - len(str(self.root)) - len(str(relative)) - 2))
        source = first / relative
        self.assertEqual(len(str(source)), 248)
        source.parent.mkdir(parents=True)
        source.write_bytes(b"long shader")
        with mock.patch("shader_cache.WINDOWS_PATH_LIMIT", 240):
            with self.assertRaisesRegex(ValueError, "240-character Windows limit"):
                self.pool.publish(self.identity, first)
        self.assertFalse(self.pool.entry(self.identity).exists())

    def test_short_profiles_publish_nonzero_and_warm_even_with_deep_evidence_root(self):
        parent = Path(ROOT.anchor) / "eawr-sc/t" if os.name == "nt" else self.root
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="sc-", dir=parent)
        self.addCleanup(temporary.cleanup)
        short = Path(temporary.name)
        evidence = PinnedGodotEvidence(sys.executable, self.root / ("staged-" + "r" * 60) / "out/godot",
                                       short / "c", ADAPTERS, profile_root=short / "u")
        relative = Path("shader_cache/BestFitNormalShaderRD") / ("a" * 64) / ("b" * 40 + ".vulkan.cache")
        rows = []
        with context_popen_hooks():
            evidence.install()
            for index in range(2):
                result = self.root / f"short-result-{index}.json"
                code = (
                    "import json,os,pathlib; "
                    "u=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
                    "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer'; "
                    f"s=u/{str(relative)!r}; l=u/'logs/godot.log'; "
                    f"pathlib.Path({str(result)!r}).write_text(json.dumps([s.exists(),l.exists(),len(str(s))])); "
                    "[(p.parent.mkdir(parents=True,exist_ok=True),p.write_bytes(b'cached')) for p in (s,l)]")
                process = subprocess.run([sys.executable, "-c", code, "--path", str(self.project)], timeout=30)
                self.assertEqual(process.returncode, 0)
                rows.append(json.loads(result.read_text()))
        self.assertEqual([r[:2] for r in rows], [[False, False], [True, False]])
        if os.name == "nt":
            self.assertTrue(all(r[2] <= 240 for r in rows))
        self.assertTrue(all(r["published_bytes"] > 0 for r in evidence.cache_records))
        self.assertGreater(evidence.cache_records[1]["seeded_bytes"], 0)
        self.assertEqual(len({r["live_root"] for r in evidence.profile_records}), 2)
        for record in evidence.profile_records:
            self.assertTrue(record.get("copied_back"), record)
            self.assertFalse(Path(record["live_root"]).exists())
            snapshot = Path(record["evidence_root"]) / ("Godot" if os.name == "nt" else "godot") / "app_userdata/EAWR Viewer"
            self.assertEqual((snapshot / "logs/godot.log").read_bytes(), b"cached")
            self.assertFalse((snapshot / "shader_cache").exists())

    def test_fault_report_paths_open_retained_files_after_short_profile_cleanup(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "evidence", self.root / "cache",
                                       ADAPTERS, profile_root=self.root / "short")
        report = self.root / "fault.json"
        external = self.root / "external.png"
        external.write_bytes(b"external capture")
        code = """import json,os,pathlib,sys
profile=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])
user=profile/('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer'
files=[user/'logs/fault.eawr-replay',user/'logs/godot.log',user/'captures/fault.png',user/'dumps/fault.dmp']
for path,payload in zip(files,[b'EAWRPLY fault',b'fault log',b'fault capture',b'fault dump']):
 path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(payload)
document={'live_session':{'failure_replay':files[0].as_posix()},
 'files':[{'log':str(files[1]).upper() if os.name=='nt' else str(files[1])},
          {'capture':str(files[2]),'crash_dump':str(files[3])}],
 'indexed':{str(files[2]):'frame'},'external':sys.argv[-1],
 'neighbor':str(profile)+'-neighbor/still.json','captures':{'configured':'unchanged hash'},
 'backend':{'adapter_name':'Test GPU'},'tick':7,'ok':False}
pathlib.Path(sys.argv[sys.argv.index('--eawr-report')+1]).write_text(json.dumps(document),encoding='utf-8')
sys.exit(2)
"""
        with context_popen_hooks():
            evidence.install()
            process = subprocess.run([sys.executable, "-c", code, "--path", str(self.project), "--",
                                      "--eawr-report", str(report), str(external)], timeout=30)
        self.assertEqual(process.returncode, 2)
        record = evidence.profile_records[0]
        self.assertTrue(record.get("copied_back"), record)
        self.assertEqual(record["reports_rewritten"], 1)
        self.assertFalse(Path(record["live_root"]).exists())
        document = json.loads(report.read_text(encoding="utf-8"))
        paths = [document["live_session"]["failure_replay"], document["files"][0]["log"],
                 document["files"][1]["capture"], document["files"][1]["crash_dump"]]
        for path, payload in zip(paths, (b"EAWRPLY fault", b"fault log", b"fault capture", b"fault dump")):
            with self.subTest(path=path):
                self.assertTrue(Path(path).is_relative_to(Path(record["evidence_root"])))
                self.assertEqual(Path(path).read_bytes(), payload)
        self.assertEqual(next(iter(document["indexed"])), paths[2])
        self.assertEqual(Path(document["external"]).read_bytes(), b"external capture")
        self.assertEqual(document["neighbor"], record["live_root"] + "-neighbor/still.json")
        self.assertEqual(document["captures"], {"configured": "unchanged hash"})
        self.assertEqual((document["tick"], document["ok"]), (7, False))
        self.assertEqual(evidence.adapters(), {"Test GPU": 1})

    def test_report_dot_components_keep_outside_paths_untouched(self):
        report = self.root / "boundary.json"
        evidence = PinnedGodotEvidence(sys.executable, self.root / "evidence", cache_enabled=False,
                                       profile_root=self.root / "short")
        code = """import json,os,pathlib,sys
profile=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])
inside=profile/'logs/fault.eawr-replay'
inside.parent.mkdir(parents=True);inside.write_bytes(b'EAWRPLY fault')
outside=profile/'..'/'outside.bin';outside.write_bytes(b'external evidence')
document={'inside':str(profile)+'/./logs/../logs/fault.eawr-replay',
 'escaped_value':str(outside),'escaped_key':{str(outside):'external'},
 'plain_outside':os.path.normpath(str(outside)),'neighbor':str(profile)+'-neighbor/outside.bin'}
pathlib.Path(sys.argv[sys.argv.index('--eawr-report')+1]).write_text(json.dumps(document),encoding='utf-8')
sys.exit(2)
"""
        with context_popen_hooks():
            evidence.install()
            process = subprocess.run([sys.executable, "-c", code, "--", "--eawr-report", str(report)], timeout=30)
        self.assertEqual(process.returncode, 2)
        record = evidence.profile_records[0]
        original = str(Path(record["live_root"]) / ".." / "outside.bin")
        document = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(Path(document["inside"]).read_bytes(), b"EAWRPLY fault")
        self.assertEqual(Path(document["inside"]), Path(record["evidence_root"]) / "logs/fault.eawr-replay")
        self.assertFalse(Path(record["live_root"]).exists())
        self.assertEqual(Path(os.path.normpath(original)).read_bytes(), b"external evidence")
        self.assertEqual(document["escaped_value"], original)
        self.assertEqual(next(iter(document["escaped_key"])), original)
        self.assertEqual(document["plain_outside"], os.path.normpath(original))
        self.assertEqual(document["neighbor"], record["live_root"] + "-neighbor/outside.bin")
        self.assertTrue(record.get("copied_back"), record)

    def test_map_report_outside_short_profile_remains_byte_identical(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "evidence", self.root / "cache",
                                       ADAPTERS, profile_root=self.root / "short")
        report = self.root / "map.json"
        contents = '{ "map": "data/art/maps/land.ted", "captures": {"configured":"hash"}, "ticks": 0 }\n'
        code = (
            "import os,pathlib,sys; "
            "p=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
            "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer/shader_cache/type/a.cache'; "
            "p.parent.mkdir(parents=True);p.write_bytes(b'shader'); "
            f"pathlib.Path(sys.argv[sys.argv.index('--eawr-report')+1]).write_bytes({contents.encode()!r})")
        with context_popen_hooks():
            evidence.install()
            process = subprocess.run([sys.executable, "-c", code, "--path", str(self.project), "--",
                                      "--eawr-report", str(report)], timeout=30)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(report.read_bytes(), contents.encode())
        self.assertEqual(evidence.profile_records[0]["reports_rewritten"], 0)
        self.assertTrue(evidence.profile_records[0]["copied_back"])
        self.assertFalse(Path(evidence.profile_records[0]["live_root"]).exists())
        self.assertGreater(evidence.cache_records[0]["published_bytes"], 0)

    def test_short_index_collision_never_reuses_another_identity(self):
        user = self.root / "collision-source"
        self.files(user)
        self.pool.publish(self.identity, user)
        entry = self.pool.entry(self.identity)
        other = dict(self.identity, collision="other")
        with mock.patch.object(self.pool, "entry", return_value=entry):
            self.assertEqual(self.pool.seed(other, self.root / "collision-target"), 0)
            self.assertEqual(self.pool.publish(other, user), 14)
            self.assertEqual(json.loads((entry / "identity.json").read_text()), other)

    def test_non_viewer_probe_profiles_retain_private_state_without_shader_bins(self):
        # The standalone probes merged with this runner use their own Godot
        # application names, so their shader paths need the same copy filter.
        (self.project / "project.godot").write_text('[application]\nconfig/name="EAWR Fog Renderer Probe"\n')
        parent = Path(ROOT.anchor) / "eawr-sc/t" if os.name == "nt" else self.root
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="sc-", dir=parent)
        self.addCleanup(temporary.cleanup)
        short = Path(temporary.name)
        evidence = PinnedGodotEvidence(sys.executable, self.root / ("staged-" + "r" * 60) / "out/godot",
                                       short / "c", ADAPTERS, profile_root=short / "u")
        shader = "shader_cache/type/" + "a" * 64 + "/" + "b" * 40 + ".vulkan.cache"
        code = (
            "import os,pathlib; "
            "u=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
            "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Fog Renderer Probe'; "
            f"[(p.parent.mkdir(parents=True,exist_ok=True),p.write_bytes(b'probe')) for p in "
            f"(u/{shader!r},u/'vulkan/pipelines.device.cache',u/'logs/godot.log',"
            "u/'logs/fault.eawr-replay',u/'state/shader_cache/private.json')]")
        with context_popen_hooks():
            evidence.install()
            completed = subprocess.run([sys.executable, "-c", code, "--path", str(self.project)], timeout=30)
        self.assertEqual(completed.returncode, 0)
        profile = evidence.profile_records[0]
        self.assertNotIn("error", profile)
        self.assertTrue(profile["copied_back"])
        self.assertFalse(Path(profile["live_root"]).exists())
        user = (Path(profile["evidence_root"]) / ("Godot" if os.name == "nt" else "godot") /
                "app_userdata/EAWR Fog Renderer Probe")
        self.assertFalse((user / "shader_cache").exists())
        self.assertFalse((user / "vulkan/pipelines.device.cache").exists())
        for path in ("logs/godot.log", "logs/fault.eawr-replay", "state/shader_cache/private.json"):
            self.assertEqual((user / path).read_bytes(), b"probe")

    def test_each_compatibility_dimension_misses_the_existing_cache(self):
        user = self.root / "user"
        self.files(user)
        self.pool.publish(self.identity, user)
        for field in ("godot_version", "engine", "gui_engine", "project", "viewer_dlls", "adapters", "rendering"):
            identity = copy.deepcopy(self.identity)
            identity[field] = "different"
            self.assertEqual(self.pool.seed(identity, self.root / field), 0, field)
        before = self.identity
        self.dll.write_bytes(b"viewer-two")
        self.assertNotEqual(before, fingerprint(Path(sys.executable), self.project, ADAPTERS, []))
        self.assertIsNone(fingerprint(Path(sys.executable), self.project, [], []))
        self.assertIsNone(fingerprint(Path(sys.executable), self.project, [{"Name": "unknown"}], []))

    def test_total_cap_evicts_old_keys_and_rejects_an_oversize_launch(self):
        identities = [dict(self.identity, revision=i) for i in range(3)]
        for index, identity in enumerate(identities):
            user = self.root / str(index)
            self.files(user, b"s" * 12, b"p" * 12)
            self.pool.publish(identity, user)
        self.assertEqual(self.pool.seed(identities[0], self.root / "old"), 0)
        self.assertEqual(self.pool.seed(identities[2], self.root / "recent"), 24)
        self.assertLessEqual(sum(p.stat().st_size for p in self.pool.root.rglob('*.cache')), 40)
        user = self.root / "large"
        self.files(user, b"s" * 41)
        self.assertEqual(self.pool.publish(self.identity, user), 0)

    def test_foreign_manifest_and_directory_links_are_refused(self):
        user = self.root / "user"
        self.files(user)
        self.pool.publish(self.identity, user)
        entry = self.pool.entry(self.identity)
        (entry / "identity.json").write_text('{"foreign":true}')
        self.assertEqual(self.pool.seed(self.identity, self.root / "foreign"), 0)
        if os.name != "nt":
            (user / "shader_cache/link").symlink_to(self.root, target_is_directory=True)
            with self.assertRaises(ValueError):
                self.pool.publish(self.identity, user)

    def test_successful_children_reuse_only_cache_with_communicate_wait_and_poll(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "profiles", self.root / "host", ADAPTERS)
        # Restore Popen after installing the real runner hooks; no renderer is launched.
        with context_popen_hooks():
            evidence.install()
            rows = []
            for index, method in enumerate(("communicate", "wait", "poll", "communicate")):
                if index == 3:
                    self.dll.write_bytes(b"changed")
                result = self.root / f"result-{index}.json"
                code = (
                    "import json, os, pathlib, sys; "
                    "u=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
                    "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer'; "
                    "s=u/'shader_cache/a.cache'; p=u/'vulkan/pipelines.device.cache'; l=u/'logs/godot.log'; "
                    f"pathlib.Path({str(result)!r}).write_text(json.dumps([s.exists(),p.exists(),l.exists(),str(u)])); "
                    "[(f.parent.mkdir(parents=True,exist_ok=True),f.write_bytes(b'cached')) for f in (s,p,l)]")
                process = subprocess.Popen([sys.executable, "-c", code, "--path", str(self.project)])
                if method == "communicate":
                    process.communicate(timeout=30)
                elif method == "wait":
                    process.wait(timeout=30)
                else:
                    deadline = time.monotonic() + 30
                    while process.poll() is None and time.monotonic() < deadline:
                        time.sleep(.01)
                self.assertEqual(process.returncode, 0)
                rows.append(json.loads(result.read_text()))
            self.assertEqual([r[:3] for r in rows], [[False, False, False], [True, True, False],
                                                    [True, True, False], [False, False, False]])
            self.assertEqual(len({r[3] for r in rows}), 4)

    def test_benchmark_groups_isolate_caches_and_explicit_cold_mode_bypasses_them(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "profiles", self.root / "host", ADAPTERS)
        argv = [sys.executable, "--path", str(self.project), "--", "--eawr-load-bench-cache", "pair-a"]
        first = evidence.prepare_cache(argv, None, self.root / "first")
        pool, identity, user, _ = first
        self.files(user)
        pool.publish(identity, user)
        second = evidence.prepare_cache(argv, None, self.root / "second")
        self.assertEqual(second[-1]["seeded_bytes"], 14)
        other = evidence.prepare_cache(argv[:-1] + ["pair-b"], None, self.root / "other")
        self.assertEqual(other[-1]["seeded_bytes"], 0)
        self.assertEqual(list((self.root / "host").glob('*')), [])
        evidence.cache_enabled = False
        self.assertIsNone(evidence.prepare_cache(argv, None, self.root / "cold"))
        self.assertFalse((self.root / "cold").exists())

    def test_failed_children_do_not_publish(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "profiles", self.root / "host", ADAPTERS)
        with context_popen_hooks():
            evidence.install()
            command = [sys.executable, "-c",
                       "import os,pathlib; u=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
                       "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer/shader_cache'; "
                       "u.mkdir(parents=True); (u/'a.cache').write_bytes(b'failed'); raise SystemExit(1)",
                       "--path", str(self.project)]
            completed = subprocess.run(command, timeout=30)
            self.assertEqual(completed.returncode, 1)
            self.assertEqual(evidence.cache_records[0]["published_bytes"], 0)
            self.assertEqual(list((self.root / "host").glob('*/shader_cache')), [])

    def test_exit_wait_finishes_before_cache_and_profile_maintenance(self):
        evidence = PinnedGodotEvidence(sys.executable, self.root / "profiles", self.root / "host",
                                       ADAPTERS, profile_root=self.root / "short")
        with context_popen_hooks():
            evidence.install()
            code = (
                "import os,pathlib; "
                "u=pathlib.Path(os.environ['APPDATA' if os.name=='nt' else 'XDG_DATA_HOME'])/"
                "('Godot' if os.name=='nt' else 'godot')/'app_userdata/EAWR Viewer'; "
                "[(p.parent.mkdir(parents=True,exist_ok=True),p.write_bytes(b'cached')) "
                "for p in (u/'shader_cache/a.cache',u/'logs/godot.log')]")
            with mock.patch.object(evidence, "publish_cache", wraps=evidence.publish_cache) as maintenance:
                process = subprocess.Popen([sys.executable, "-c", code, "--path", str(self.project)])
                self.assertEqual(process._eawr_wait_for_exit(timeout=30), 0)
                maintenance.assert_not_called()
                profile = evidence.profile_records[0]
                self.assertTrue(Path(profile["live_root"]).exists())
                self.assertFalse(profile.get("copied_back", False))
                self.assertEqual(evidence.cache_records[0]["published_bytes"], 0)
                # Evidence finalization remains synchronous when requested
                # after the process-exit timer has stopped.
                self.assertEqual(process.wait(timeout=30), 0)
                maintenance.assert_called_once_with(process)
                self.assertGreater(evidence.cache_records[0]["published_bytes"], 0)
                self.assertTrue(profile["copied_back"])
                self.assertFalse(Path(profile["live_root"]).exists())
                self.assertEqual((Path(profile["evidence_root"]) / ("Godot" if os.name == "nt" else "godot") /
                                  "app_userdata/EAWR Viewer/logs/godot.log").read_bytes(), b"cached")

    def test_abrupt_publisher_exit_releases_lock_and_recovers(self):
        user = self.root / "source"
        self.files(user)
        for stage, exit_code in (("identity", 74), ("binary", 73)):
            with self.subTest(stage=stage):
                pool = ShaderCachePool(self.root / stage)
                # Kill a real lock-holding process, rather than raising an
                # exception that would run the publisher's cleanup handlers.
                code = (
                    "import os,pathlib,sys; "
                    f"sys.path.insert(0,{str(ROOT / 'tools/rig')!r}); "
                    "from shader_cache import ShaderCachePool; "
                    "replace=pathlib.Path.replace\n"
                    "def interrupt(source,target):\n"
                    f" if {stage!r}=='identity' and target.name=='identity.json': os._exit(74)\n"
                    " result=replace(source,target)\n"
                    f" if {stage!r}=='binary' and target.suffix=='.cache': os._exit(73)\n"
                    " return result\n"
                    "pathlib.Path.replace=interrupt\n"
                    f"ShaderCachePool(pathlib.Path({str(pool.root)!r})).publish("
                    f"{self.identity!r},pathlib.Path({str(user)!r}))\n")
                process = subprocess.run([sys.executable, "-c", code], timeout=30)
                self.assertEqual(process.returncode, exit_code)
                seeded = pool.seed(self.identity, self.root / f"{stage}-partial")
                self.assertEqual(seeded, 0 if stage == "identity" else 6)
                self.assertEqual(pool.publish(self.identity, user), 14)
                recovered = self.root / f"{stage}-recovered"
                self.assertEqual(pool.seed(self.identity, recovered), 14)
                self.assertEqual((recovered / "shader_cache/type/a.cache").read_bytes(), b"shader")
                self.assertEqual((recovered / "vulkan/pipelines.device.driver.cache").read_bytes(), b"pipeline")
                self.assertEqual(list(pool.root.rglob("*.tmp")), [])

    def test_legacy_missing_or_corrupt_manifest_retains_runner_publication(self):
        argv = [sys.executable, "--path", str(self.project)]
        for index, damaged in enumerate((None, '{"schema":')):
            with self.subTest(manifest=damaged):
                host = self.root / f"host-{index}"
                evidence = PinnedGodotEvidence(sys.executable, self.root / "profiles", host, ADAPTERS)
                pool = ShaderCachePool(host)
                entry = pool.entry(self.identity)
                entry.mkdir(exist_ok=True)
                self.files(entry)
                if damaged is not None:
                    # Reproduce the on-disk state of the previous version's
                    # interrupted truncate/rewrite in an abruptly killed child.
                    manifest = entry / "identity.json"
                    manifest.write_text(json.dumps(self.identity))
                    code = (
                        "import os,pathlib; "
                        f"stream=pathlib.Path({str(manifest)!r}).open('w'); "
                        f"stream.write({damaged!r}); stream.flush(); os._exit(74)")
                    self.assertEqual(subprocess.run([sys.executable, "-c", code], timeout=30).returncode, 74)
                pending = evidence.prepare_cache(argv, None, self.root / f"launch-{index}")
                self.assertIsNotNone(pending)
                recovered_pool, identity, user, record = pending
                self.assertEqual(record["seeded_bytes"], 0)
                self.assertNotIn("error", record)
                self.files(user)
                process = mock.Mock(returncode=0, _eawr_cache=pending, _eawr_short_profile=None)
                evidence.publish_cache(process)
                self.assertEqual(record["published_bytes"], 14)
                self.assertEqual(recovered_pool.seed(identity, self.root / f"warm-{index}"), 14)
                # A compatible manifest is immutable on later publications;
                # the old truncate/rewrite interruption point no longer exists.
                manifest = entry / "identity.json"
                with mock.patch.object(ShaderCachePool, "write_identity", side_effect=AssertionError("rewritten")):
                    self.assertEqual(recovered_pool.publish(identity, user), 14)
                self.assertEqual(json.loads(manifest.read_text()), identity)


def context_popen_hooks():
    import contextlib
    stack = contextlib.ExitStack()
    for name in ("__init__", "communicate", "wait", "poll"):
        stack.enter_context(mock.patch.object(subprocess.Popen, name, getattr(subprocess.Popen, name)))
    return stack


if __name__ == "__main__":
    unittest.main()
