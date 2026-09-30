import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from build_manifest_test_support import *


class StrictJsonTests(BuilderTestCase):
    def test_duplicate_keys(self):
        for target in ("report", "supplement", "contract", "derivation"):
            with self.subTest(target=target):
                fixture = self.fixture("prototype", target)
                path = getattr(fixture, target)
                raw = path.read_text(encoding="utf-8")
                path.write_text(raw.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1', 1),
                                encoding="utf-8")
                if target == "derivation":
                    fixture.rebind_supplement()
                self.assert_refused(fixture, 1, "duplicate object key 'schema_version'")

    def test_non_finite_numbers(self):
        for constant in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(constant=constant):
                fixture = self.fixture("production", constant.strip("-"))
                raw = fixture.report.read_text(encoding="utf-8")
                fixture.report.write_text(
                    raw.replace('"animation_time_seconds": 0,', f'"animation_time_seconds": {constant},', 1),
                    encoding="utf-8")
                self.assert_refused(fixture, 1, "non-finite JSON number")

    def test_overflowing_and_oversized_numbers(self):
        huge = "1" + "0" * 400
        cases = {
            # Checked fields: 1e999 used to parse to inf; 10**400 used to raise
            # an uncaught OverflowError in float() and break the receipt.
            "checked_float": ("production", "report", '"far": 20000', '"far": 1e999'),
            "checked_negative_float": ("production", "report", '"far": 20000', '"far": -1e999'),
            "checked_huge_int": ("production", "report", '"far": 20000', f'"far": {huge}'),
            # Ignored fields were previously accepted silently.
            "ignored_float": ("prototype", "report", '"draw_calls_last_frame": 1',
                              '"draw_calls_last_frame": 1e999'),
            "ignored_huge_int": ("prototype", "report", '"draw_calls_last_frame": 1',
                                 f'"draw_calls_last_frame": {huge}'),
            "ignored_just_over_int64": ("prototype", "report", '"draw_calls_last_frame": 1',
                                        f'"draw_calls_last_frame": {2 ** 63}'),
            "contract": ("production", "contract", '"pixel_channel_threshold": 0',
                         '"pixel_channel_threshold": 1e999'),
            "supplement": ("production", "supplement", '"schema_version": 1', f'"schema_version": {huge}'),
            "derivation": ("prototype", "derivation", '"particle_seed": 0', f'"particle_seed": {huge}'),
        }
        for name, (host, target, old, new) in cases.items():
            with self.subTest(case=name):
                fixture = self.fixture(host, name)
                path = getattr(fixture, target)
                raw = path.read_text(encoding="utf-8")
                self.assertIn(old, raw)
                path.write_text(raw.replace(old, new, 1), encoding="utf-8")
                if target == "derivation":
                    fixture.rebind_supplement()
                fragment = "signed 64-bit range" if huge in new or str(2 ** 63) in new else "non-finite"
                receipt = self.assert_refused(fixture, 1, fragment)
                self.assertIn("invalid JSON", receipt["errors"][0])

        with self.assertRaises(build_manifest.EvidenceError):
            build_manifest._number(10 ** 400, "direct")

    def test_overflow_keeps_one_structured_cli_receipt(self):
        fixture = self.fixture("production")
        raw = fixture.report.read_text(encoding="utf-8")
        fixture.report.write_text(raw.replace('"far": 20000', '"far": ' + "9" * 400, 1), encoding="utf-8")
        result = subprocess.run([sys.executable, str(TOOL), *fixture.argv()],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        receipt = json.loads(result.stdout)  # exactly one JSON document
        self.assertEqual((receipt["status"], receipt["exit_code"]), ("failed", 1))
        self.assertFalse(receipt["manifest"]["written"])
        self.assertFalse(fixture.output.exists())

    def test_byte_order_mark_and_invalid_json(self):
        fixture = self.fixture("production", "bom")
        fixture.report.write_bytes(b"\xef\xbb\xbf" + fixture.report.read_bytes())
        self.assert_refused(fixture, 1, "byte-order mark")
        fixture = self.fixture("production", "truncated")
        fixture.report.write_bytes(fixture.report.read_bytes()[:-5])
        self.assert_refused(fixture, 1, "invalid JSON")


class EvidenceTests(BuilderTestCase):
    def test_missing_evidence_files(self):
        for option in ("report", "scene", "replay", "image", "supplement", "build_receipt", "runtime_receipt"):
            with self.subTest(option=option):
                fixture = self.fixture("production", option)
                self.assert_refused(fixture, 1, "is missing", **{option: fixture.evidence / "absent.json"})

    def test_scene_and_replay_are_hashed_independently(self):
        fixture = self.fixture("production", "scene")
        fixture.scene.write_bytes(fixture.scene.read_bytes() + b" ")
        self.assert_refused(fixture, 1, "--scene SHA-256")
        fixture = self.fixture("production", "replay")
        fixture.replay.write_bytes(fixture.replay.read_bytes()[:-1])
        self.assert_refused(fixture, 1, "--replay SHA-256")

    def test_contract_identity_must_equal_frozen_identity(self):
        cases = {
            "tick": lambda c: c["identity"].update(simulation_tick=0),
            "profile": lambda c: c["identity"].update(vfs_profile="remake"),
            "input": lambda c: c["identity"]["inputs"][0].update(sha256="3" * 64),
            "camera": lambda c: c["identity"]["camera"].update(far=10000.0),
        }
        for name, mutate in cases.items():
            with self.subTest(case=name):
                fixture = self.fixture("production", name)
                value = contract()
                mutate(value)
                write_json(fixture.contract, value)
                self.assert_refused(fixture, 1, "frozen P1-01 fixed-scene identity")

    def test_invalid_contract_schema(self):
        fixture = self.fixture("production")
        value = contract()
        value["regions"] = []
        write_json(fixture.contract, value)
        self.assert_refused(fixture, 1, "contract.regions")

    def test_image_must_be_decodable_png_at_viewport_size(self):
        fixture = self.fixture("prototype", "size")
        draw_png(fixture.image, size=(640, 360))
        value = fixture.derivation_value()
        fixture.write_derivation(value)
        fixture.rebind_supplement()
        self.assert_refused(fixture, 1, "decoded dimensions 640x360")

        fixture = self.fixture("production", "truncated")
        raw = fixture.image.read_bytes()
        fixture.image.write_bytes(raw[: len(raw) // 2])
        self.assert_refused(fixture, 1, "cannot decode PNG")

        fixture = self.fixture("production", "not-png")
        Image.new("RGBA", (1280, 720)).save(fixture.image, format="BMP")
        self.assert_refused(fixture, 1, "not a PNG")

    def test_production_capture_hash_must_match_image(self):
        fixture = self.fixture("production")
        draw_png(fixture.image, size=(1280, 720))
        image = Image.open(fixture.image).convert("RGBA")
        image.putpixel((0, 0), (255, 0, 0, 255))
        image.save(fixture.image, format="PNG")
        self.assert_refused(fixture, 1, "report.capture_sha256")


class PathPolicyTests(BuilderTestCase):
    def test_existing_output_is_never_overwritten(self):
        fixture = self.fixture("production")
        fixture.output.write_text("original\n", encoding="utf-8")
        code, receipt, _ = run_builder(fixture.argv())
        self.assertEqual(code, 2, receipt)
        self.assertIn("refusing to overwrite", receipt["errors"][0])
        self.assertEqual(fixture.output.read_text(encoding="utf-8"), "original\n")

    def test_output_must_be_beside_image_and_json(self):
        fixture = self.fixture("production", "elsewhere")
        self.assert_refused(fixture, 2, "same directory as --image", output=fixture.evidence / "manifest.json")
        self.assertFalse((fixture.evidence / "manifest.json").exists())
        fixture = self.fixture("production", "suffix")
        self.assert_refused(fixture, 2, "must end with .json", output=fixture.run / "manifest.txt")

    def test_output_aliasing_an_input_is_refused(self):
        fixture = self.fixture("production")
        self.assert_refused(fixture, 2, "refusing to overwrite", output=fixture.report)
        self.assertTrue(fixture.report.is_file())

    def test_input_aliases_are_refused(self):
        fixture = self.fixture("production", "same-path")
        self.assert_refused(fixture, 2, "aliases --build-receipt", runtime_receipt=fixture.build_receipt)

        fixture = self.fixture("production", "case-alias")
        alias = str(fixture.build_receipt).upper() if os.name == "nt" else None
        if alias is not None:
            self.assert_refused(fixture, 2, "aliases --build-receipt", runtime_receipt=alias)

        fixture = self.fixture("production", "hard-link")
        link = fixture.evidence / "runtime-hardlink.json"
        try:
            os.link(fixture.build_receipt, link)
        except OSError:
            self.skipTest("hard links are unavailable on this filesystem")
        self.assert_refused(fixture, 2, "aliases --build-receipt", runtime_receipt=link)

    def test_traversal_drive_relative_and_unc_are_refused(self):
        fixture = self.fixture("production")
        traversal = str(fixture.run / ".." / "evidence" / "supplement.json")
        self.assert_refused(fixture, 2, "parent traversal", supplement=traversal)
        self.assert_refused(fixture, 2, "drive-relative", report="C:report.json")
        self.assert_refused(fixture, 2, "UNC", report="\\\\server\\share\\report.json")
        self.assert_refused(fixture, 2, "UNC", report="//server/share/report.json")
        self.assert_refused(fixture, 2, "UNC", image="\\\\?\\C:\\frame.png")
        self.assert_refused(fixture, 2, "parent traversal", output=str(fixture.run / ".." / "run" / "m.json"))

    def test_alternate_data_stream_paths_are_refused(self):
        fixture = self.fixture("production")
        streams = False
        if os.name == "nt":
            probe = fixture.evidence / "stream-probe.txt"
            probe.write_bytes(b"probe")
            try:
                with open(f"{probe}:probe", "wb") as handle:
                    handle.write(b"x")
                streams = True
            except OSError:
                pass
        image_bytes = fixture.image.read_bytes()
        image_mtime = os.stat(fixture.image).st_mtime_ns
        image_stream = f"{fixture.image}:manifest.json"
        for output in (image_stream, f"{fixture.image}:manifest.json:$DATA", f"{fixture.output}::$DATA"):
            with self.subTest(output=output):
                self.assert_refused(fixture, 2, "alternate data stream", output=output)
        self.assertEqual(fixture.image.read_bytes(), image_bytes)
        self.assertEqual(os.stat(fixture.image).st_mtime_ns, image_mtime)
        if streams:
            with self.assertRaises(FileNotFoundError):
                open(image_stream, "rb").close()

        # An input read from a stream of another file is refused the same way.
        carrier = fixture.evidence / "carrier.txt"
        carrier.write_text("carrier\n", encoding="utf-8")
        report_stream = f"{carrier}:report.json"
        if streams:
            with open(report_stream, "wb") as handle:
                handle.write(fixture.report.read_bytes())
        self.assert_refused(fixture, 2, "alternate data stream", report=report_stream)

    def test_ambiguous_windows_spellings_are_refused(self):
        fixture = self.fixture("production")
        cases = {
            "output_device": ({"output": fixture.run / "nul.json"}, "device name"),
            "output_device_upper": ({"output": fixture.run / "CON.json"}, "device name"),
            "input_device": ({"report": fixture.evidence / "aux.json"}, "device name"),
            "input_device_superscript": ({"report": fixture.evidence / "lpt¹.json"}, "device name"),
            "output_directory_trailing_dot": ({"output": f"{fixture.run}./manifest.json"}, "dot or space"),
            "input_trailing_space": ({"report": f"{fixture.report} "}, "dot or space"),
            "input_trailing_dot": ({"report": f"{fixture.report}."}, "dot or space"),
            "output_wildcard": ({"output": fixture.run / "man*fest.json"}, "reserved Windows path character"),
            "input_question": ({"report": fixture.evidence / "report?.json"}, "reserved Windows path character"),
        }
        for name, (overrides, fragment) in cases.items():
            with self.subTest(case=name):
                self.assert_refused(fixture, 2, fragment, **overrides)
        self.assertEqual(sorted(path.name for path in fixture.run.iterdir()), ["frame.png", "report.json"])

    def test_symlinked_inputs_and_directories_are_refused(self):
        fixture = self.fixture("production")
        link = fixture.evidence / "report-link.json"
        try:
            os.symlink(fixture.report, link)
        except (OSError, NotImplementedError):
            self.skipTest("symlink creation is not permitted on this host")
        self.assert_refused(fixture, 2, "reparse point", report=link)

        directory_link = self.root / "linked-run"
        os.symlink(fixture.run, directory_link, target_is_directory=True)
        self.assert_refused(fixture, 2, "reparse point", image=directory_link / "frame.png",
                            output=directory_link / "manifest.json")

    def test_usage_errors(self):
        fixture = self.fixture("production")
        self.assert_refused(fixture, 2, "command line", role="reviewer")
        self.assert_refused(fixture, 2, "command line", host="linux")
        self.assert_refused(fixture, 2, "command line", supplement=None)
