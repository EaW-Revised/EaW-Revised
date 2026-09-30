import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from windows_capture_test_support import *


class SyntheticObservationTests(unittest.TestCase):
    def assert_failed(self, assessment, fragment):
        self.assertEqual(assessment["status"], "failed")
        self.assertTrue(any(fragment in failure for failure in assessment["failures"]),
                        (fragment, assessment["failures"]))

    def test_corroborated_tree(self):
        record, assessment = observe()
        self.assertEqual(assessment["status"], "corroborated", assessment["failures"])
        self.assertEqual(assessment["engine"]["pid"], 200)
        self.assertEqual(assessment["engine"]["ppid"], 100)
        self.assertEqual(assessment["engine_exit"]["exit_code"], 0)
        events = assessment["events"]
        self.assertLess(events["last_before_png"]["t_end_mono_ns"], events["png_first_seen"]["t_mono_ns"])
        self.assertGreater(events["first_after_png"]["t_start_mono_ns"], events["png_first_seen"]["t_mono_ns"])
        self.assertFalse(record["last_before_png"]["png_exists_after_snapshot"])
        labels = {row["label"] for row in record["extension_hashes"]}
        self.assertEqual(labels, {"first_extension_seen", "first_after_png"})
        self.assertTrue(all(row["holder_alive_after_hash"] for row in record["extension_hashes"]))

    def test_extension_never_loaded(self):
        _, assessment = observe({"extension_from": None})
        self.assert_failed(assessment, "exactly one process holding the extension, found 0")
        self.assert_failed(assessment, "no process handle was held")

    def test_extension_loaded_only_after_png(self):
        _, assessment = observe({"extension_from": 2.5})
        self.assert_failed(assessment, "before PNG: expected exactly one process holding the extension")

    def test_extension_from_another_path(self):
        _, assessment = observe({"extension_path": "C:/elsewhere/" + DLL_NAME})
        self.assert_failed(assessment, "loaded extension path differs from the staged extension")

    def test_loaded_extension_hash_mismatch(self):
        _, assessment = observe(hash_value="b" * 64)
        self.assert_failed(assessment, "loaded extension file hash differs")

    def test_png_never_appears(self):
        _, assessment = observe({"png_at": None})
        self.assert_failed(assessment, "capture PNG never appeared")
        self.assert_failed(assessment, "no complete snapshot was taken after")

    def test_png_present_before_launch(self):
        _, assessment = observe({"png_at": 0.0})
        self.assert_failed(assessment, "no complete snapshot was taken before")

    def test_engine_instance_replaced_around_png(self):
        _, assessment = observe({"engine_restart_at": 2.0})
        self.assertEqual(assessment["status"], "failed")

    def test_two_extension_holders(self):
        _, assessment = observe({"second_holder": True})
        self.assert_failed(assessment, "found 2")

    def test_session_parent_and_arguments(self):
        self.assert_failed(observe({"engine_session": 2})[1], "engine session differs")
        self.assert_failed(observe(session_id=3)[1], "root process session differs")
        self.assert_failed(observe({"engine_argv": ARGV[1:-1]})[1], "engine arguments differ")
        self.assert_failed(observe(root_command_line="other")[1], "root process command line differs")
        self.assert_failed(observe(root_image="C:/fake/other.exe")[1], "root process image differs")
        self.assert_failed(observe(engine_image="C:/fake/other.exe")[1], "engine image differs")

    def test_module_list_unavailable(self):
        _, assessment = observe({"module_error": True})
        self.assert_failed(assessment, "module list unavailable")

    def test_reused_pid_is_not_grafted_into_tree(self):
        record, assessment = observe({"reused_pid_child": True})
        self.assertEqual(assessment["status"], "corroborated", assessment["failures"])
        pids = {row["pid"] for row in record["last_before_png"]["processes"]}
        self.assertNotIn(400, pids)

    def test_replacement_with_another_parent_is_never_owned(self):
        record, assessment, world = observe_world({"root_end": 999.0, "stale_parent_replacement": True}, timeout=3.0)
        (replacement,) = rows_for(world, 500)
        for key in ("last_before_png", "first_after_png"):
            self.assertNotIn(500, {row["pid"] for row in record[key]["processes"]})
        self.assertFalse(replacement["killed"], "a replacement parented elsewhere was terminated")
        self.assertTrue(world.alive(replacement))
        self.assertNotIn(500, {entry["pid"] for entry in record["termination"]["entries"]})
        self.assertNotIn(500, record["terminated_pids"])
        self.assertEqual(sorted(record["terminated_pids"]), [100, 200])
        self.assertTrue(record["termination"]["complete"])
        self.assert_failed(assessment, "observation timed out")

    def test_both_ownership_paths_reject_mismatched_or_unavailable_parent(self):
        clock = FakeClock()
        world = FakeWorld(clock, root_end=999.0, stale_parent_replacement=True)
        clock.sleep(1.0)
        engine = world.row(200)
        self.assertEqual(world.process_table()[500]["ppid"], 200)  # the stale table names the engine
        snapshot = observer.snapshot_tree(world, 100, DLL_NAME)
        self.assertEqual({row["pid"] for row in snapshot["processes"]}, {100, 200})
        candidates = {(200, engine["creation"]): 100}
        observer._add_live_descendants(world, world.process_table(), candidates)
        self.assertEqual(candidates, {(200, engine["creation"]): 100})

        # Even an instance that genuinely is the engine's child is refused when
        # its parent cannot be read from the same handle as its creation time.
        world.row(500)["ppid"] = 200
        identity = world.identity

        def without_parent(pid):
            value = identity(pid)
            return value if value is None or pid != 500 else dict(value, parent_pid=None)

        with mock.patch.object(world, "identity", side_effect=without_parent):
            snapshot = observer.snapshot_tree(world, 100, DLL_NAME)
            observer._add_live_descendants(world, world.process_table(), candidates)
        self.assertEqual({row["pid"] for row in snapshot["processes"]}, {100, 200})
        self.assertEqual(candidates, {(200, engine["creation"]): 100})
        # With the parent readable and matching, the same child is adopted.
        self.assertIn(500, {row["pid"] for row in observer.snapshot_tree(world, 100, DLL_NAME)["processes"]})

    def test_loaded_extension_base_and_image_size_are_retained(self):
        record, assessment = observe()
        self.assertEqual(assessment["status"], "corroborated", assessment["failures"])
        for key in ("last_before_png", "first_after_png"):
            (engine,) = [row for row in record[key]["processes"] if row["pid"] == 200]
            self.assertEqual(engine["extension_module_images"], [{
                "path": EXTENSION, "base_address": EXTENSION_BASE, "image_size": EXTENSION_IMAGE_SIZE,
                "image_error": None}])
        for row in record["extension_hashes"]:
            self.assertEqual((row["base_address"], row["image_size"], row["pe_size_of_image"]),
                             (EXTENSION_BASE, EXTENSION_IMAGE_SIZE, EXTENSION_IMAGE_SIZE))
        self.assertEqual(assessment["engine"]["extension_module_images"][0]["image_size"], EXTENSION_IMAGE_SIZE)

    def test_loaded_extension_image_unavailable(self):
        _, assessment = observe({"image_error": True})
        self.assert_failed(assessment, "before PNG: loaded extension module base address or image size unavailable")
        self.assert_failed(assessment, "after PNG: loaded extension module base address or image size unavailable")

    def test_loaded_extension_remapped_between_snapshots(self):
        _, assessment = observe({"image_moved_at": 2.0})
        self.assert_failed(assessment, "base address or image size changed between the before- and after-PNG")
        self.assert_failed(assessment, "first_extension_seen: hashed module base address or image size differs")

    def test_loaded_image_size_differs_from_pe_header(self):
        _, assessment = observe(image_size_of=lambda _path: EXTENSION_IMAGE_SIZE + 0x1000)
        self.assert_failed(assessment, "image size differs from the hashed file's PE SizeOfImage")

    def test_pe_header_unreadable(self):
        def unreadable(_path):
            raise ValueError("not a PE image (no MZ header)")

        record, assessment = observe(image_size_of=unreadable)
        self.assert_failed(assessment, "PE SizeOfImage of the hashed extension file unavailable")
        self.assertTrue(all(row["pe_size_of_image"] is None for row in record["extension_hashes"]))

    def test_pe_size_of_image_reads_real_header(self):
        if WINDOWS:
            self.assertGreater(observer.pe_size_of_image(str(extension_source())), 0)
        with tempfile.TemporaryDirectory() as temporary:
            bogus = pathlib.Path(temporary) / "bogus.dll"
            bogus.write_bytes(b"MZ" + b"\0" * 100)
            with self.assertRaises(ValueError):
                observer.pe_size_of_image(str(bogus))

    def test_timeout_terminates_tree(self):
        record, assessment, world = observe_world({"root_end": 999.0}, timeout=3.0)
        self.assertTrue(record["timed_out"])
        self.assertEqual(sorted(record["terminated_pids"]), [100, 200])
        self.assertTrue(rows_for(world, 200)[0]["killed"] and rows_for(world, 100)[0]["killed"])
        self.assertTrue(record["termination"]["complete"])
        self.assertTrue(record["engine_handle"]["terminate_access"])
        (engine_entry,) = [entry for entry in record["termination"]["entries"] if entry.get("engine")]
        self.assertEqual((engine_entry["via"], engine_entry["result"]), ("held handle", "terminated"))
        self.assert_failed(assessment, "observation timed out")

    def test_termination_never_touches_a_reused_pid(self):
        record, assessment, world = observe_world(
            {"root_end": 999.0, "helper_child": True, "reuse_on_terminate_open": {300: 2.9}}, timeout=3.0)
        original, replacement = rows_for(world, 300)
        self.assertTrue(replacement.get("reused"))
        self.assertFalse(replacement["killed"], "an unrelated process that reused the PID was terminated")
        self.assertTrue(world.alive(replacement))
        self.assertEqual(termination_results(record)[(300, original["creation"])], "identity_mismatch")
        self.assertNotIn(300, record["terminated_pids"])
        self.assertEqual(sorted(record["terminated_pids"]), [100, 200])
        self.assertTrue(rows_for(world, 200)[0]["killed"])
        self.assertTrue(record["termination"]["complete"])
        self.assert_failed(assessment, "observation timed out")

    def test_vanished_launcher_with_surviving_engine(self):
        record, assessment, world = observe_world({"root_end": 1.0, "engine_end": 999.0})
        self.assertTrue(record["timed_out"])
        engine = rows_for(world, 200)[0]
        self.assertTrue(engine["killed"], "the orphaned engine survived the observer")
        results = termination_results(record)
        self.assertEqual(results[(200, engine["creation"])], "terminated")
        self.assertEqual(results[(100, None)], "already_exited")
        self.assertEqual(record["terminated_pids"], [200])
        self.assertTrue(record["termination"]["complete"])
        self.assert_failed(assessment, "observation timed out")

    def test_enumeration_failure_with_surviving_engine(self):
        record, assessment, world = observe_world(
            {"root_end": 1.0, "engine_end": 999.0, "table_fails_from": 1.5})
        self.assertTrue(record["snapshot_errors"])
        self.assertTrue(rows_for(world, 200)[0]["killed"])
        self.assertEqual(record["terminated_pids"], [200])
        self.assertTrue(record["termination"]["enumeration_errors"])
        self.assertFalse(record["termination"]["complete"])
        self.assert_failed(assessment, "termination was not confirmed complete")

    def test_engine_that_does_not_exit_is_not_claimed(self):
        record, assessment, world = observe_world({"root_end": 1.0, "engine_end": 999.0, "unkillable": {200}})
        engine = rows_for(world, 200)[0]
        self.assertEqual(termination_results(record)[(200, engine["creation"])], "exit_not_observed")
        self.assertNotIn(200, record["terminated_pids"])
        self.assertEqual(record["termination"]["failures"], [{"pid": 200, "result": "exit_not_observed"}])
        self.assert_failed(assessment, "1 instance(s) not confirmed exited")

    def test_engine_without_terminate_access_is_not_claimed(self):
        record, assessment, world = observe_world({"root_end": 1.0, "engine_end": 999.0, "deny_terminate": True})
        self.assertFalse(record["engine_handle"]["terminate_access"])
        engine = rows_for(world, 200)[0]
        self.assertFalse(engine["killed"])
        self.assertEqual(termination_results(record)[(200, engine["creation"])], "open_failed")
        self.assertNotIn(200, record["terminated_pids"])
        self.assert_failed(assessment, "termination was not confirmed complete")

    def test_observer_error_terminates_owned_tree(self):
        clock = FakeClock()
        world = FakeWorld(clock, root_end=999.0, engine_end=999.0)

        def broken_hash(_path):
            raise RuntimeError("hash backend exploded")

        with self.assertRaises(observer.ObservationError) as caught:
            observer.observe_launch(FakeProcess(world), world, "frame.png", DLL_NAME, interval_seconds=0.05,
                                    timeout_seconds=30.0, clock=clock, png_exists=world.png_exists,
                                    hash_file=broken_hash, image_size_of=lambda _path: EXTENSION_IMAGE_SIZE)
        record = caught.exception.record
        json.dumps(record)
        self.assertIsInstance(caught.exception.__cause__, RuntimeError)
        self.assertIn("RuntimeError", record["observer_error"])
        self.assertEqual(record["termination"]["reason"], "observer_error")
        self.assertTrue(record["termination"]["complete"])
        self.assertEqual(sorted(record["terminated_pids"]), [100, 200])
        self.assertTrue(rows_for(world, 100)[0]["killed"] and rows_for(world, 200)[0]["killed"])
        assessment = observer.assess_observation(record, expected(), split=lambda text: text.split(SEP))
        self.assert_failed(assessment, "process observation raised an error")


# ------------------------------------------------------------------ argv policy

OLD_ARGV = ["C:/godot/console.exe", "--rendering-method", "gl_compatibility", "--path", "C:/old/project", "--",
            "--eawr-game-root", "C:/game", "--eawr-mod-root", "C:/mod", "--eawr-scene", "C:/repo/scene.json",
            "--eawr-replay", "C:/repo/replay", "--eawr-capture", "C:/old/p0-1/frame.png",
            "--eawr-report", "C:/old/p0-1/report.json", "--eawr-benchmark"]


class ArgvPolicyTests(unittest.TestCase):
    def test_rewrite_changes_only_three_values(self):
        new = capture.rewrite_argv(OLD_ARGV, "C:/new/project", "C:/new/p0-1/frame.png", "C:/new/p0-1/report.json")
        changed = [(old, value) for old, value in zip(OLD_ARGV, new) if old != value]
        self.assertEqual(changed, [("C:/old/project", "C:/new/project"),
                                   ("C:/old/p0-1/frame.png", "C:/new/p0-1/frame.png"),
                                   ("C:/old/p0-1/report.json", "C:/new/p0-1/report.json")])
        self.assertEqual(len(new), len(OLD_ARGV))

    def test_refused_layouts(self):
        cases = {
            "headless probe": OLD_ARGV + ["--eawr-headless-probe"],
            "closeup": OLD_ARGV + ["--eawr-closeup-capture", "x.png"],
            "resize probe": OLD_ARGV + ["--eawr-resize-probe"],
            "engine headless": OLD_ARGV[:1] + ["--headless"] + OLD_ARGV[1:],
            "editor": OLD_ARGV[:1] + ["--editor", "x"] + OLD_ARGV[1:],
            "duplicate": OLD_ARGV + ["--eawr-benchmark"],
            "missing report": OLD_ARGV[:16] + OLD_ARGV[18:],
            "two separators": OLD_ARGV + ["--"],
            "renderer": [item if item != "gl_compatibility" else "forward_plus" for item in OLD_ARGV],
            "empty": OLD_ARGV[:4] + [""] + OLD_ARGV[5:],
            "not a list": "C:/godot/console.exe --path x",
        }
        for label, argv in cases.items():
            with self.subTest(label):
                with self.assertRaises(capture.EvidenceError):
                    capture.parse_argv(argv, label)


# ------------------------------------------------------- real Windows process

FAKE_CONSOLE = r'''
import os, subprocess, sys, time
engine = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_engine.py")
command = [sys.executable, engine, *sys.argv[1:]]
flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
orphan_after = os.environ.get("EAWR_FAKE_ORPHAN_AFTER")
if orphan_after:
    # Launcher vanishes while the engine keeps running.
    subprocess.Popen(command, stdout=sys.stdout, stderr=sys.stderr, creationflags=flags)
    time.sleep(float(orphan_after))
    sys.exit(0)
sys.exit(subprocess.call(command, stdout=sys.stdout, stderr=sys.stderr, creationflags=flags))
'''

FAKE_ENGINE = r'''
import ctypes, os, shutil, sys, time
args = sys.argv[1:]
value = lambda name: args[args.index(name) + 1]
mode = os.environ.get("EAWR_FAKE_MODE", "normal")
dll = os.path.join(value("--path"), "bin", "%s")
if mode == "other-dll":
    ctypes.WinDLL(os.environ["EAWR_FAKE_OTHER_DLL"])
elif mode != "no-dll":
    ctypes.WinDLL(dll)
time.sleep(float(os.environ.get("EAWR_FAKE_DELAY", "0.8")))
from PIL import Image
Image.new("RGBA", (1280, 720), (8, 8, 16, 255)).save(value("--eawr-capture"))
print("%s", flush=True)
time.sleep(float(os.environ.get("EAWR_FAKE_DELAY", "0.8")))
report = os.environ.get("EAWR_FAKE_REPORT")
if report and "--eawr-report" in args:
    shutil.copyfile(report, value("--eawr-report"))
''' % (DLL_NAME, DEVICE_LINE)


def write_fake_engine(directory):
    (directory / "fake_console.py").write_text(FAKE_CONSOLE, encoding="utf-8")
    (directory / "fake_engine.py").write_text(FAKE_ENGINE, encoding="utf-8")
    return directory / "fake_console.py", directory / "fake_engine.py"


def extension_source():
    return pathlib.Path(sys.base_prefix) / "python3.dll"


@unittest.skipUnless(WINDOWS, "Windows process observation")
class LiveObserverTests(unittest.TestCase):
    """Observes a real, harmless two-level Python process tree."""

    def test_real_tree_is_corroborated(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            console, engine = write_fake_engine(base)
            (base / "project/bin").mkdir(parents=True)
            extension = base / "project/bin" / DLL_NAME
            shutil.copyfile(extension_source(), extension)
            png = base / "frame.png"
            argv = [sys.executable, str(console), "--path", str(base / "project"), "--",
                    "--eawr-capture", str(png)]
            with open(base / "stdout.log", "xb") as out, open(base / "stderr.log", "xb") as err:
                process = observer.launch_hidden(argv, out, err, str(base))
                record = observer.observe_launch(process, observer.WindowsProcessProbe(), str(png), DLL_NAME,
                                                 interval_seconds=0.02, timeout_seconds=60)
                self.assertEqual(process.wait(timeout=30), 0)
            probe = observer.WindowsProcessProbe()
            assessment = observer.assess_observation(record, {
                "root_pid": process.pid, "root_image": sys.executable, "engine_image": sys.executable,
                "extension_path": str(extension), "extension_sha256": sha256_file(extension),
                "session_id": probe._session(os.getpid()), "root_command_line": subprocess.list2cmdline(argv),
                "argv_tail": [str(engine)] + argv[2:]})
            self.assertEqual(assessment["status"], "corroborated", assessment["failures"])
            self.assertEqual(assessment["engine"]["ppid"], process.pid)
            self.assertEqual(assessment["engine_exit"]["exit_code"], 0)
            self.assertEqual(assessment["engine"]["visible_top_level_windows"], 0)
            self.assertEqual(json.loads(json.dumps(record))["root_pid"], process.pid)
            (image,) = assessment["engine"]["extension_module_images"]
            self.assertGreater(image["base_address"], 0)
            self.assertEqual(image["image_size"], observer.pe_size_of_image(str(extension)))
            self.assertIsNone(record["termination"])

    def test_orphaned_engine_is_terminated_through_the_held_handle(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            console, _ = write_fake_engine(base)
            (base / "project/bin").mkdir(parents=True)
            shutil.copyfile(extension_source(), base / "project/bin" / DLL_NAME)
            argv = [sys.executable, str(console), "--path", str(base / "project"), "--",
                    "--eawr-capture", str(base / "frame.png")]
            probe = observer.WindowsProcessProbe()
            record = None
            with mock.patch.dict(os.environ, {"EAWR_FAKE_ORPHAN_AFTER": "1.5", "EAWR_FAKE_DELAY": "60",
                                              "EAWR_FAKE_MODE": "normal"}):
                with open(base / "stdout.log", "xb") as out, open(base / "stderr.log", "xb") as err:
                    process = observer.launch_hidden(argv, out, err, str(base))
                    try:
                        record = observer.observe_launch(process, probe, str(base / "frame.png"), DLL_NAME,
                                                         interval_seconds=0.02, timeout_seconds=60,
                                                         exit_grace_seconds=1.0, terminate_wait_seconds=10)
                    finally:
                        engine = (record or {}).get("engine_handle")
                        survivor = engine and probe.open_for_termination(engine["pid"])
                        if survivor:  # never leave a harmless test process running
                            try:
                                if survivor.times()[0] == engine["creation_filetime"] and not survivor.has_exited():
                                    survivor.terminate(1)
                                    survivor.wait(10)
                                    self.fail("the orphaned engine survived the observer")
                            finally:
                                survivor.close()
                        process.wait(timeout=30)
            self.assertTrue(record["timed_out"])
            engine = record["engine_handle"]
            self.assertTrue(engine["terminate_access"])
            self.assertTrue(engine["exited"])
            self.assertEqual(engine["exit_code"], 1)
            self.assertIn(engine["pid"], record["terminated_pids"])
            self.assertNotIn(process.pid, record["terminated_pids"])  # the launcher had already exited
            self.assertTrue(record["termination"]["complete"], record["termination"])
