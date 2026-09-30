import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from build_manifest_test_support import *


class SuccessTests(BuilderTestCase):
    def test_prototype_and_production_build_valid_manifests(self):
        for host in ("prototype", "production"):
            with self.subTest(host=host):
                fixture = self.fixture(host)
                code, receipt, text = run_builder(fixture.argv())
                self.assertEqual(code, 0, receipt)
                self.assertEqual(receipt["status"], "assembled")
                self.assertIs(receipt["visual_comparison"], False)
                self.assertIs(receipt["acceptance"], False)
                self.assertTrue(receipt["human_review_gaps"])
                self.assertIn("runtime.build_revision", receipt["field_sources"])
                self.assertEqual(receipt["inputs"]["image"]["sha256"], fixture.image_sha256)
                self.assertEqual(receipt["image"], {"format": "PNG", "mode": "RGBA",
                                                    "width": 1280, "height": 720})
                self.assertNotIn(str(self.root), text)
                self.assertNotIn(self.root.name, text)

                raw = fixture.output.read_bytes()
                self.assertFalse(raw.startswith(b"\xef\xbb\xbf"))
                self.assertEqual(receipt["manifest"]["sha256"], sha256(raw))
                manifest = json.loads(raw)
                self.assertEqual(manifest["image"], {"path": "frame.png", "sha256": fixture.image_sha256})
                self.assertEqual(manifest["identity"], build_manifest.FROZEN_IDENTITY)
                self.assertEqual(manifest["contract_sha256"], sha256(fixture.contract.read_bytes()))
                self.assertEqual(manifest["runtime"], {
                    "platform": "windows",
                    "architecture": "x86_64",
                    "engine": "Godot" if host == "prototype" else "Godot 4.7.2-stable",
                    "engine_version": "4.7.2-stable",
                    "backend": "gl_compatibility",
                    "graphics_api": GL_API,
                    "device": f"{VENDOR} {ADAPTER}",
                    "driver": VENDOR,
                    "driver_version": DRIVER_VERSION,
                    "build_revision": REVISION,
                })
                self.assertNotIn("Z:/synthetic", raw.decode("utf-8"))

                validate = subprocess.run(
                    [sys.executable, str(COMPARE), "validate", "--contract", str(fixture.contract),
                     "--manifest", str(fixture.output)],
                    capture_output=True, text=True, check=False)
                self.assertEqual(validate.returncode, 0, validate.stderr)

    def test_output_is_deterministic(self):
        first = self.fixture("production", "first")
        second = self.fixture("production", "second")
        self.assertEqual(run_builder(first.argv())[0], 0)
        self.assertEqual(run_builder(second.argv())[0], 0)
        self.assertEqual(first.output.read_bytes(), second.output.read_bytes())

    def test_built_pair_passes_same_backend_preflight(self):
        prototype = self.fixture("prototype")
        production = self.fixture("production")
        # Both runs must bind the same reviewed contract bytes.
        shutil.copyfile(prototype.contract, production.contract)
        self.assertEqual(run_builder(prototype.argv())[0], 0)
        self.assertEqual(run_builder(production.argv())[0], 0)
        result = subprocess.run(
            [sys.executable, str(PREFLIGHT), "check", "--mode", "same-backend",
             "--contract", str(prototype.contract), "--baseline", str(prototype.output),
             "--candidate", str(production.output)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIs(json.loads(result.stdout)["visual_comparison"], False)

    def test_command_line_entry_point(self):
        fixture = self.fixture("prototype")
        result = subprocess.run([sys.executable, str(TOOL), *fixture.argv()],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(json.loads(result.stdout)["status"], "assembled")
        self.assertTrue(fixture.output.is_file())


class ProductionDriftTests(BuilderTestCase):
    def mutate(self, mutate, fragment, code=1):
        fixture = self.fixture("production")
        report = production_report(fixture.image_sha256)
        mutate(report)
        fixture.write_report(report)
        self.assert_refused(fixture, code, fragment)

    def test_identity_and_status_drift(self):
        cases = {
            "status": (lambda r: r.update(status="failed"), "report.status"),
            "failure": (lambda r: r.update(failure="boom"), "report.failure"),
            "tick": (lambda r: r["capture_identity"].update(simulation_tick=0), "simulation_tick"),
            "frame": (lambda r: r["capture_identity"].update(capture_frame=119), "capture_frame"),
            "profile": (lambda r: r["capture_identity"].update(vfs_profile="vanilla"), "vfs_profile"),
            "camera": (lambda r: r["capture_identity"]["camera"].update(position=[0, 421, 1050]), "position[1]"),
            "fov": (lambda r: r["capture_identity"]["camera"].update(fov_degrees=50), "fov_degrees"),
            "viewport": (lambda r: r["capture_identity"]["viewport"].update(width=1920), "viewport.width"),
            "animation": (lambda r: r["animation"].update(logical_path="data/art/models/x.ala"), "animation"),
            "animation_time": (lambda r: r.update(animation_time_seconds=0.5), "animation_time_seconds"),
            "model": (lambda r: r["model"].update(sha256="c" * 64), "report.model.sha256"),
            "texture_path": (lambda r: r["texture"].update(logical_path="data/art/textures/other.dds"),
                             "report.texture.logical_path"),
            "scene": (lambda r: r.update(scene_sha256="d" * 64), "report.scene_sha256"),
            "replay": (lambda r: r["replay"].update(sha256="e" * 64), "report.replay.sha256"),
            "replay_name": (lambda r: r["replay"].update(logical_path="C:/x/other.eawr-replay"),
                            "report.replay.logical_path"),
            "capture_hash": (lambda r: r.update(capture_sha256="f" * 64), "report.capture_sha256"),
            "snapshots": (lambda r: r.update(snapshot_count=5), "snapshot_count"),
            "renderer": (lambda r: r["backend"].update(rendering_method="forward_plus"), "rendering_method"),
            "headless": (lambda r: r["backend"].update(adapter_name=""), "adapter_name"),
            "engine": (lambda r: r["backend"].update(engine="Godot 4.7.1-stable"), "report.backend.engine"),
            "runtime_exercise": (lambda r: r.update(renderer_runtime_exercise=True), "renderer_runtime_exercise"),
            "camera_mode": (lambda r: r.update(tactical_camera_verified=True), "tactical_camera_verified"),
            "camera_section": (lambda r: r.update(tactical_camera={}), "tactical_camera"),
            "map_mode": (lambda r: r.update(mode="map"), "not a fixed-scene capture report"),
            "unknown": (lambda r: r.update(extra_field=1), "unsupported extra_field"),
            "hull_preview": (lambda r: r.update(model_preview={"exploratory": True, "acceptance": False}),
                             "unsupported model_preview"),
            "missing": (lambda r: r.pop("pass_order"), "missing pass_order"),
            "pass_order": (lambda r: r.update(pass_order=["opaque", "transparent"]), "pass_order"),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(case=name):
                self.mutate(mutate, fragment)

    def test_bool_as_int_is_rejected(self):
        cases = {
            "particle_seed": (lambda r: r["capture_identity"].update(particle_seed=False), "particle_seed"),
            "tick": (lambda r: r["capture_identity"].update(simulation_tick=True), "simulation_tick"),
            "switches": (lambda r: r.update(runtime_scene_switch_count=False), "runtime_scene_switch_count"),
            "camera": (lambda r: r["capture_identity"]["camera"]["up"].__setitem__(1, True), "up[1]"),
            "schema": (lambda r: r.update(schema_version=True), "schema_version"),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(case=name):
                self.mutate(mutate, fragment)


class PrototypeDriftTests(BuilderTestCase):
    def mutate(self, mutate, fragment, code=1):
        fixture = self.fixture("prototype")
        report = prototype_report()
        mutate(report)
        fixture.write_report(report)
        self.assert_refused(fixture, code, fragment)

    def test_modes_probes_and_drift(self):
        cases = {
            "status": (lambda r: r.update(status="failed_no_draw_calls"), "report.status"),
            "closeup": (lambda r: r["capture"].update(closeup_done=True), "closeup_done"),
            "camera_not_restored": (lambda r: r["capture"].update(baseline_camera_restored_before_capture=False),
                                    "baseline_camera_restored_before_capture"),
            "control_probe": (lambda r: r["controls"].update(probe_requested=True), "probe_requested"),
            "probe_exercised": (lambda r: r["controls"].update(probe_exercised=True), "probe_exercised"),
            "resize_probe": (lambda r: r["settings"].update(resize_probe=True), "resize_probe"),
            "headless": (lambda r: r["platform"].update(headless=True), "headless"),
            "vsync_headless": (lambda r: r["settings"].update(vsync_observed_mode=-1), "vsync_observed_mode"),
            "linux": (lambda r: r["platform"].update(os="Linux"), "report.platform.os"),
            "resolution": (lambda r: r["settings"].update(resolution=[960, 540]), "resolution"),
            "fixed_frame": (lambda r: r["settings"].update(fixed_camera_frame=119), "fixed_camera_frame"),
            "renderer": (lambda r: r["backend"].update(rendering_method="forward_plus"), "rendering_method"),
            "scene": (lambda r: r["scene_hash"].update(scene_json="1" * 64), "scene_json"),
            "texture": (lambda r: r["scene_hash"].update(texture="2" * 64), "scene_hash.texture"),
            "snapshots": (lambda r: r["snapshot"].update(snapshot_count=5), "snapshot_count"),
            "replay_hashes": (lambda r: r["snapshot"].update(replay_hashes=["a" * 64]), "replay_hashes"),
            "vendor_placeholder": (lambda r: r["hardware"].update(adapter_vendor="unknown"), "adapter_vendor"),
            "unknown": (lambda r: r.update(extra=True), "unsupported extra"),
            "stock_material": (lambda r: r["material"].update(stock_material_substitute=True),
                               "stock_material_substitute"),
            "bool_vsync_mode": (lambda r: r["settings"].update(vsync_observed_mode=True), "vsync_observed_mode"),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(case=name):
                self.mutate(mutate, fragment)

    def test_derivation_receipt_is_required_and_bound(self):
        fixture = self.fixture("prototype", "missing")
        self.assert_refused(fixture, 2, "required for --host prototype", prototype_capture_derivation=None)

        cases = {
            "tick": ({"simulation_tick": 0}, "simulation_tick"),
            "bool_tick": ({"simulation_tick": True}, "simulation_tick"),
            "frame": ({"capture_process_frame_zero_based": 120}, "capture_process_frame_zero_based"),
            "revision": ({"source_revision": "f" * 40}, "source_revision"),
            "capture": ({"capture_sha256": "0" * 64}, "capture_sha256"),
            "replay": ({"replay_sha256": "0" * 64}, "replay_sha256"),
            "source_path": ({"source_logical_path": "../prototype_host.cpp"}, "source_logical_path"),
            "particle": ({"particle_time_seconds": 1.0}, "particle_time_seconds"),
        }
        for name, (changes, fragment) in cases.items():
            with self.subTest(case=name):
                fixture = self.fixture("prototype", "derivation-" + name)
                value = fixture.derivation_value()
                value.update(changes)
                fixture.write_derivation(value)
                fixture.rebind_supplement()
                self.assert_refused(fixture, 1, fragment)

    def test_derivation_receipt_hash_mismatch(self):
        fixture = self.fixture("prototype")
        value = fixture.derivation_value()
        value["rationale"] = "edited after the supplement was written"
        fixture.write_derivation(value)
        self.assert_refused(fixture, 1, "capture_derivation_receipt_sha256")

    def test_production_rejects_derivation_receipt(self):
        fixture = self.fixture("production")
        derivation = fixture.evidence / "capture-derivation.json"
        derivation.write_text("{}", encoding="utf-8")
        self.assert_refused(fixture, 2, "only valid for --host prototype",
                            prototype_capture_derivation=derivation)


class SupplementTests(BuilderTestCase):
    def test_supplement_fields(self):
        cases = {
            "linux": ({"platform": "linux"}, "supplement.platform"),
            "arm64": ({"architecture": "arm64"}, "supplement.architecture"),
            "short_revision": ({"build_revision": "0123456"}, "build_revision"),
            "upper_revision": ({"build_revision": REVISION.upper()}, "build_revision"),
            "zero_revision": ({"build_revision": "0" * 40}, "build_revision"),
            "gl_string_driver": ({"driver_version": GL_API}, "driver_version"),
            "placeholder_driver": ({"driver_version": "unknown"}, "driver_version"),
            "host": ({"host": "production"}, "supplement.host"),
            "bool_schema": ({"schema_version": True}, "schema_version"),
            "kind": ({"kind": "other"}, "supplement.kind"),
            "build_hash": ({"build_receipt_sha256": "1" * 64}, "build_receipt_sha256"),
            "runtime_hash": ({"runtime_receipt_sha256": "1" * 64}, "runtime_receipt_sha256"),
            "extra": ({"clean_build": True}, "unsupported clean_build"),
        }
        for name, (changes, fragment) in cases.items():
            with self.subTest(case=name):
                fixture = self.fixture("prototype", name)
                fixture.rebind_supplement(**changes)
                self.assert_refused(fixture, 1, fragment)

    def test_production_supplement_must_not_carry_derivation_hash(self):
        fixture = self.fixture("production")
        fixture.rebind_supplement(capture_derivation_receipt_sha256="1" * 64)
        self.assert_refused(fixture, 1, "unsupported capture_derivation_receipt_sha256")

    def test_receipt_content_changed_after_binding(self):
        fixture = self.fixture("production")
        fixture.build_receipt.write_text("tampered\n", encoding="utf-8")
        self.assert_refused(fixture, 1, "build_receipt_sha256")

    def test_empty_and_duplicate_receipts(self):
        fixture = self.fixture("production", "empty")
        fixture.build_receipt.write_bytes(b"")
        fixture.rebind_supplement()
        self.assert_refused(fixture, 1, "receipt file is empty")

        fixture = self.fixture("production", "same-content")
        fixture.runtime_receipt.write_bytes(fixture.build_receipt.read_bytes())
        fixture.rebind_supplement()
        self.assert_refused(fixture, 1, "must be distinct evidence")
