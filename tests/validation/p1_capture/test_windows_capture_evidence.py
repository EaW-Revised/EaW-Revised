import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from windows_capture_test_support import *
from test_windows_capture_process import extension_source, write_fake_engine


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=fixture", "-c", "user.email=fixture@invalid",
                           "-c", "commit.gpgsign=false", *arguments], check=True, capture_output=True, text=True)


def synthetic_host(adapter_name="AMD Radeon RX 7900 XTX"):
    session = observer.WindowsProcessProbe()._session(os.getpid())
    return {
        "computer_name": "SYNTHETIC", "machine_guid_sha256": "c" * 64, "architecture": "AMD64",
        "windows_version": {"major": 10, "minor": 0, "build": 26200}, "registry": {"UBR": 1},
        "os": {"BuildNumber": "26200", "LastBootUpTimeUtc": "2026-01-01T00:00:00.0000000Z"},
        "user": {"Name": "synthetic", "Sid": "S-1-5-21-0-0-0-1001"},
        "observer_session_id": session, "powershell_session_id": session,
        "adapters": [{"Name": adapter_name, "DriverVersion": "32.0.31041.3013", "PNPDeviceID": "PCI\\SYNTHETIC",
                      "AdapterCompatibility": "Advanced Micro Devices, Inc."}],
    }


@unittest.skipUnless(WINDOWS, "the capture helper is Windows only")
class CaptureCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.temporary.name).resolve()
        self.repo = self.base / "repo"
        project = self.repo / "prototypes/godot/project"
        (project / "bin").mkdir(parents=True)
        (project / ".godot").mkdir()
        (project / "out").mkdir()
        (project / "common").mkdir()
        (self.repo / ".gitignore").write_text("out/\n", encoding="utf-8")
        (project / ".gitignore").write_text("/bin/\n/common/\n/.godot/\n/out/\n", encoding="utf-8")
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (project / "main.tscn").write_text("[gd_scene format=3]\n", encoding="utf-8")
        (project / capture.GDEXTENSION_FILE).write_text(
            "[libraries]\n\nwindows.debug.x86_64 = \"res://bin/%s\"\nwindows.release.x86_64 = \"res://bin/%s\"\n"
            % (DLL_NAME, DLL_NAME), encoding="utf-8")
        shutil.copyfile(extension_source(), project / "bin" / DLL_NAME)
        (project / ".godot/cache.bin").write_bytes(b"editor cache")
        (project / "out/old.png").write_bytes(b"old output")
        (project / "common/scene.json").write_text("{}", encoding="utf-8")
        scene = self.repo / "prototypes/common/scene.json"
        replay = self.repo / "tests/replay/fixtures/original-v1.eawr-replay"
        scene.parent.mkdir(parents=True)
        replay.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "prototypes/common/scene.json", scene)
        shutil.copyfile(ROOT / "tests/replay/fixtures/original-v1.eawr-replay", replay)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "synthetic prototype")
        self.revision = git(self.repo, "rev-parse", "HEAD").stdout.strip()
        self.game = self.base / "game"
        self.mod = self.base / "mod"
        self.game.mkdir()
        self.mod.mkdir()
        self.fake_directory = self.base / "fake"
        self.fake_directory.mkdir()
        self.console, self.engine = write_fake_engine(self.fake_directory)
        import test_build_manifest

        self.report_template = self.base / "report-template.json"
        self.report_template.write_text(json.dumps(test_build_manifest.prototype_report()), encoding="utf-8")
        self.prior = self.repo / "out/renderer-acceptance/prior"
        self.write_prior()
        self.parent = self.repo / "out/renderer-acceptance"
        self.environment = mock.patch.dict(os.environ, {"EAWR_FAKE_REPORT": str(self.report_template),
                                                        "EAWR_FAKE_MODE": "normal", "EAWR_FAKE_DELAY": "0.6"})
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temporary.cleanup()

    def old_argv(self, run, extra=()):
        prior = self.prior
        return [sys.executable, "--rendering-method", "gl_compatibility", "--path", str(prior / "project"), "--",
                "--eawr-game-root", str(self.game), "--eawr-mod-root", str(self.mod),
                "--eawr-scene", str(self.repo / "prototypes/common/scene.json"),
                "--eawr-replay", str(self.repo / "tests/replay/fixtures/original-v1.eawr-replay"),
                "--eawr-capture", str(prior / run / "frame.png"), "--eawr-report", str(prior / run / "report.json"),
                "--eawr-benchmark", *extra]

    def write_prior(self, argv_extra=(), **receipt_changes):
        prior = self.prior
        if prior.exists():
            shutil.rmtree(prior)
        (prior / "p0-1").mkdir(parents=True)
        (prior / "p0-2").mkdir()
        project = self.repo / "prototypes/godot/project"
        receipt = {
            "source_repo": str(self.repo), "source_head": self.revision, "source_clean": True,
            "project_source": str(project), "excluded": [".godot", "out"],
            "prototype_dll_sha256": sha256_file(project / "bin" / DLL_NAME),
            "godot_console": sys.executable, "godot_console_sha256": sha256_file(sys.executable),
            "godot_version": "4.7.2.stable.official.synthetic", "game_root": str(self.game),
            "mod_root": str(self.mod),
            "scene_sha256": sha256_file(self.repo / "prototypes/common/scene.json"),
            "replay_sha256": sha256_file(self.repo / "tests/replay/fixtures/original-v1.eawr-replay"),
        }
        receipt.update(receipt_changes)
        argv_1, argv_2 = self.old_argv("p0-1", argv_extra), self.old_argv("p0-2", argv_extra)
        readiness = {
            "source": {"head": self.revision},
            "runtime": {"godot_console": sys.executable, "godot_console_sha256": sha256_file(sys.executable),
                        "godot_engine_sibling": sys.executable,
                        "godot_engine_sibling_sha256": sha256_file(sys.executable)},
            "runs": [{"name": "p0-1", "argv": argv_1}, {"name": "p0-2", "argv": argv_2}],
        }
        for path, value in ((prior / "staging-receipt.json", receipt),
                            (prior / "windows-p0-repeat-readiness.json", readiness),
                            (prior / "p0-1/argv.json", argv_1), (prior / "p0-2/argv.json", argv_2)):
            path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def cli(self, command, *extra, launcher=None, host=None, host_probe=None, probe_factory=None):
        argv = [command, "--prior-evidence", str(self.prior), "--repo", str(self.repo),
                "--expect-staging-receipt-sha256", sha256_file(self.prior / "staging-receipt.json"),
                "--expect-readiness-sha256", sha256_file(self.prior / "windows-p0-repeat-readiness.json"),
                "--poll-interval", "0.02", "--timeout", "60", *extra]
        output = io.StringIO()
        host_value = host or synthetic_host()
        with contextlib.redirect_stdout(output):
            code = capture.run(argv, launcher=launcher or self.fake_launcher,
                               host_probe=host_probe or (lambda: dict(host_value)), probe_factory=probe_factory)
        text = output.getvalue()
        return code, json.loads(text), text

    def fake_launcher(self, argv, stdout, stderr, cwd):
        launched = [argv[0], str(self.console), *argv[1:]]
        return observer.launch_hidden(launched, stdout, stderr, cwd), launched, [str(self.engine), *argv[1:]]

    def evidence_directories(self):
        return sorted(path for path in self.parent.iterdir() if path.name.startswith(capture.EVIDENCE_PREFIX))

    def assert_private_paths_absent(self, text):
        for private in (str(self.base), str(self.base).replace("\\", "/"), str(self.base).replace("\\", "\\\\")):
            self.assertNotIn(private.lower(), text.lower())

    # ----------------------------------------------------------- dry-run
    def test_dry_run_is_ready_and_writes_nothing(self):
        before = sorted(path.name for path in self.parent.iterdir())
        code, receipt, text = self.cli("dry-run")
        self.assertEqual(code, 0, receipt)
        self.assertEqual(receipt["status"], "ready")
        self.assertFalse(receipt["launched"])
        self.assertTrue(receipt["observer_self_check"]["passed"])
        self.assertEqual(receipt["source"]["prototype_revision"], self.revision)
        self.assertEqual(receipt["project_file_count"], 6)
        self.assertEqual(sorted(path.name for path in self.parent.iterdir()), before)
        self.assertIn("<new-evidence>/project", receipt["argv_template"])
        self.assertIn("<game-root>", receipt["argv_template"])
        self.assertFalse(receipt["visual_comparison"] or receipt["acceptance"])
        self.assert_private_paths_absent(text)

    def test_run_requires_explicit_live_launch(self):
        code, receipt, _ = self.cli("run")
        self.assertEqual(code, 2)
        self.assertIn("--allow-live-launch", receipt["error"])
        self.assertEqual(self.evidence_directories(), [])

    def test_dry_run_refusals(self):
        code, receipt, _ = self.cli("dry-run")
        self.assertEqual(code, 0, receipt)
        argv = ["dry-run", "--prior-evidence", str(self.prior), "--repo", str(self.repo),
                "--expect-staging-receipt-sha256", "0" * 64,
                "--expect-readiness-sha256", sha256_file(self.prior / "windows-p0-repeat-readiness.json")]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(capture.run(argv, host_probe=synthetic_host), 1)
        self.assertIn("audited hash", json.loads(output.getvalue())["error"])

        cases = [
            ("dll hash", {"prototype_dll_sha256": "d" * 64}, (), "source prototype DLL hash differs"),
            ("scene hash", {"scene_sha256": "e" * 64}, (), "scene: pre-launch hash differs"),
            ("launcher hash", {"godot_console_sha256": "f" * 64}, (), "launcher hash differs"),
            ("probe argv", {}, ("--eawr-control-probe",), "is not allowed"),
            ("other checkout", {"source_repo": str(self.base / "elsewhere")}, (), "not the audited source checkout"),
            ("game root", {"game_root": str(self.base / "missing")}, (), "argv roots differ"),
        ]
        for label, changes, argv_extra, fragment in cases:
            with self.subTest(label):
                self.write_prior(argv_extra, **changes)
                code, receipt, text = self.cli("dry-run")
                self.assertEqual(code, 1, receipt)
                self.assertIn(fragment, receipt["error"])
                self.assert_private_paths_absent(text)
        self.write_prior()

    def test_dry_run_refuses_changed_or_dirty_source(self):
        scratch = self.repo / "untracked.txt"
        scratch.write_text("dirty", encoding="utf-8")
        code, receipt, _ = self.cli("dry-run")
        self.assertEqual((code, "uncommitted or untracked" in receipt.get("error", "")), (1, True))
        scratch.unlink()
        (self.repo / "tools.txt").write_text("helper change", encoding="utf-8")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "helper only")
        code, receipt, _ = self.cli("dry-run")
        self.assertEqual(code, 0, receipt)
        self.assertNotEqual(receipt["source"]["helper_revision"], self.revision)
        self.assertEqual(receipt["source"]["prototype_revision"], self.revision)
        (self.repo / "prototypes/godot/project/main.tscn").write_text("[gd_scene format=3]\n# drift\n",
                                                                      encoding="utf-8")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "prototype drift")
        code, receipt, _ = self.cli("dry-run")
        self.assertEqual(code, 1, receipt)
        self.assertIn("changed since the audited revision", receipt["error"])

    def test_evidence_parent_must_be_ignored_and_outside_roots(self):
        code, receipt, _ = self.cli("dry-run", "--evidence-parent", str(self.repo / "prototypes"))
        self.assertEqual(code, 1)
        self.assertIn("not git-ignored", receipt["error"])
        code, receipt, _ = self.cli("dry-run", "--evidence-parent", str(self.prior / "nested"))
        self.assertEqual(code, 1)
        self.assertIn("prior evidence", receipt["error"])

    # --------------------------------------------------------------- run
    def test_run_with_fake_engine_is_corroborated(self):
        prior_hashes = {path.name: sha256_file(path) for path in self.prior.rglob("*") if path.is_file()}
        code, receipt, text = self.cli("run", "--allow-live-launch")
        self.assertEqual(code, 0, receipt)
        self.assertEqual(receipt["status"], "corroborated")
        self.assertTrue(receipt["provenance_claimed"])
        self.assertEqual([run["name"] for run in receipt["runs"]], ["p0-1", "p0-2"])
        self.assertTrue(all(run["process_provenance"] == "corroborated" for run in receipt["runs"]))
        self.assert_private_paths_absent(text)
        (evidence,) = self.evidence_directories()
        self.assertEqual(receipt["evidence_directory"], evidence.relative_to(self.repo).as_posix())
        manifest_bytes = (evidence / "artifact-manifest.json").read_bytes()
        self.assertEqual(hashlib.sha256(manifest_bytes).hexdigest(), receipt["artifact_manifest_sha256"])
        manifest = json.loads(manifest_bytes)
        self.assertEqual(manifest["status"], "corroborated")
        self.assertFalse(manifest["visual_comparison"] or manifest["acceptance"])
        self.assertEqual(manifest["source"]["prototype_revision"], self.revision)
        self.assertEqual(manifest["selected_adapter"]["DriverVersion"], "32.0.31041.3013")
        self.assertTrue(manifest["prior_evidence_unchanged"] and manifest["staged_files_unchanged"])
        self.assertEqual(manifest["pre_launch_hashes"], manifest["post_run_hashes"])
        listed = {row["path"] for row in manifest["artifacts"]}
        for run in ("p0-1", "p0-2"):
            for name in ("argv.json", "stdout.log", "stderr.log", "exit-code.txt", "report.json", "frame.png",
                         "observation.json", "process-provenance.json"):
                self.assertIn(f"{run}/{name}", listed)
        for row in manifest["artifacts"]:
            self.assertEqual(sha256_file(evidence / row["path"]), row["sha256"])
        staged = evidence / "project"
        self.assertFalse((staged / ".godot").exists() or (staged / "out").exists())
        self.assertTrue((staged / "bin" / DLL_NAME).is_file())
        old = json.loads((self.prior / "p0-1/argv.json").read_text(encoding="utf-8"))
        for run in ("p0-1", "p0-2"):
            new = json.loads((evidence / run / "argv.json").read_text(encoding="utf-8"))
            changed = {index for index, (left, right) in enumerate(zip(old, new)) if left != right}
            self.assertEqual(changed, {4, 15, 17})
            self.assertEqual(new[4], str(staged))
            self.assertEqual(new[15], str(evidence / run / "frame.png"))
            provenance = json.loads((evidence / run / "process-provenance.json").read_text(encoding="utf-8"))
            engine = provenance["assessment"]["engine"]
            self.assertEqual(engine["extension_modules"], [str(staged / "bin" / DLL_NAME)])
            self.assertTrue(observer.same_path(engine["image_path"], sys.executable))
        self.assertTrue((evidence / "p0-1/frame.png").is_file() and (evidence / "p0-2/frame.png").is_file())
        self.assertEqual(prior_hashes,
                         {path.name: sha256_file(path) for path in self.prior.rglob("*") if path.is_file()})

    def test_run_fails_closed_without_loaded_extension(self):
        os.environ["EAWR_FAKE_MODE"] = "no-dll"
        code, receipt, _ = self.cli("run", "--allow-live-launch")
        self.assertEqual(code, 1)
        self.assertFalse(receipt["provenance_claimed"])
        self.assertEqual([run["name"] for run in receipt["runs"]], ["p0-1"])
        (evidence,) = self.evidence_directories()
        manifest = json.loads((evidence / "artifact-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["status"], "failed")
        self.assertFalse(manifest["provenance_claimed"])
        self.assertTrue(any("found 0" in failure for failure in manifest["failures"]))
        self.assertTrue(any("stopped after p0-1" in failure for failure in manifest["failures"]))
        self.assertFalse((evidence / "p0-2").exists())

    def test_run_fails_closed_on_foreign_extension_copy(self):
        other = self.base / "foreign" / DLL_NAME
        other.parent.mkdir()
        shutil.copyfile(extension_source(), other)
        os.environ.update({"EAWR_FAKE_MODE": "other-dll", "EAWR_FAKE_OTHER_DLL": str(other)})
        code, receipt, _ = self.cli("run", "--allow-live-launch")
        self.assertEqual(code, 1)
        (evidence,) = self.evidence_directories()
        manifest = json.loads((evidence / "artifact-manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(any("loaded extension path differs" in failure for failure in manifest["failures"]))

    def test_run_fails_closed_when_adapter_is_not_enumerated(self):
        code, receipt, _ = self.cli("run", "--allow-live-launch", host=synthetic_host("Other Adapter"))
        self.assertEqual(code, 1)
        self.assertTrue(any("selected adapter" in failure for failure in receipt["failures"]))

    def test_run_records_inputs_deleted_or_unreadable_after_launch(self):
        host_value = synthetic_host()
        calls = []
        unreadable = {str(self.repo / "prototypes/common/scene.json")}
        real_sha256_file = capture.sha256_file

        def host_probe():
            calls.append(len(calls))
            if len(calls) == 2:  # the after-run receipt: both repeats have finished
                (evidence,) = self.evidence_directories()
                (self.prior / "p0-2/argv.json").unlink()
                (evidence / "project/main.tscn").unlink()
                unreadable.add(str(evidence / "p0-1" / "stderr.log"))
                unreadable.add("armed")
            return dict(host_value)

        def guarded_sha256_file(path):
            if "armed" in unreadable and str(path) in unreadable:
                raise PermissionError(13, "Permission denied", str(path))
            return real_sha256_file(path)

        with mock.patch.object(capture, "sha256_file", guarded_sha256_file):
            code, receipt, text = self.cli("run", "--allow-live-launch", host_probe=host_probe)
        self.assertEqual(code, 1, receipt)
        self.assertEqual(len(calls), 2)
        self.assertFalse(receipt["provenance_claimed"])
        self.assertTrue(all(run["process_provenance"] == "corroborated" for run in receipt["runs"]))
        self.assert_private_paths_absent(text)
        (evidence,) = self.evidence_directories()
        manifest_bytes = (evidence / "artifact-manifest.json").read_bytes()
        self.assertEqual(hashlib.sha256(manifest_bytes).hexdigest(), receipt["artifact_manifest_sha256"])
        manifest = json.loads(manifest_bytes)
        self.assertEqual(manifest["status"], "failed")
        failures = manifest["failures"]
        for expected_failure in (
                "scene: post-run hash unavailable (PermissionError: Permission denied)",
                "prior evidence p0-2/argv.json: post-run hash unavailable (missing)",
                "prior evidence changed during the runs",
                "staged main.tscn: missing or changed during the runs (missing)",
                "artifact p0-1/stderr.log: cannot be hashed (PermissionError)"):
            self.assertIn(expected_failure, failures)
        self.assert_private_paths_absent(json.dumps(failures))
        self.assertIsNone(manifest["post_run_hashes"]["scene"])
        self.assertEqual(manifest["post_run_hashes"]["replay"], manifest["pre_launch_hashes"]["replay"])
        self.assertFalse(manifest["prior_evidence_unchanged"] or manifest["staged_files_unchanged"])
        (stderr_row,) = [row for row in manifest["artifacts"] if row["path"] == "p0-1/stderr.log"]
        self.assertEqual((stderr_row["sha256"], stderr_row["error"]), (None, "PermissionError"))
        self.assertIn("host-after.json", {row["path"] for row in manifest["artifacts"]})

    def test_observer_error_terminates_the_tree_and_still_writes_the_manifest(self):
        calls = []

        class BrokenProbe(observer.WindowsProcessProbe):
            def modules(self, pid, attempts=5):
                result = super().modules(pid, attempts)
                if any(os.path.basename(module["path"]) == DLL_NAME for module in result[0] or []):
                    calls.append(pid)
                    raise RuntimeError("module walker exploded")
                return result

        code, receipt, text = self.cli("run", "--allow-live-launch", probe_factory=BrokenProbe)
        self.assertEqual(code, 1, receipt)
        self.assertTrue(calls)
        self.assert_private_paths_absent(text)
        self.assertTrue(any("aborted: ObservationError" in failure for failure in receipt["failures"]))
        (evidence,) = self.evidence_directories()
        manifest = json.loads((evidence / "artifact-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["status"], "failed")
        self.assertFalse((evidence / "p0-2").exists())
        record = json.loads((evidence / "p0-1/observation.json").read_text(encoding="utf-8"))
        termination = record["termination"]
        self.assertEqual(termination["reason"], "observer_error")
        self.assertTrue(termination["complete"], termination)
        self.assertIn(calls[0], record["terminated_pids"])  # the engine child found by its creation time
        (root_entry,) = [entry for entry in termination["entries"] if entry.get("root")]
        # The launcher may exit on its own once its engine child is terminated.
        self.assertIn(root_entry["result"], ("terminated", "already_exited"))
        probe = observer.WindowsProcessProbe()
        for entry in termination["entries"]:
            identity = probe.identity(entry["pid"])
            if identity is not None and entry.get("creation_filetime") is not None:
                self.assertNotEqual(identity["creation_filetime"], entry["creation_filetime"])
                continue
            self.assertTrue(identity is None or not identity["alive"] or entry.get("root"), entry)
        self.assertIn("p0-1/observation.json", {row["path"] for row in manifest["artifacts"]})
