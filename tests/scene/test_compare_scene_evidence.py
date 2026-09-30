"""Focused rejection tests for the five-target scene evidence gate."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tools.compare_scene_evidence import DEFAULT_TARGETS, compare


BUILD = "a" * 40
SCENE = (b"eawr-static-scene 1\nmap data/art/maps/eawr_scene_synthetic.ted "
         + b"b" * 64 + b" land\n")


def write_lf(path: Path, value: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(value)


class SceneEvidenceComparatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "evidence"
        self.root.mkdir()
        self.fixture = Path(self.temp.name) / "fixture.py"
        self.fixture.write_bytes(b"synthetic fixture v1\n")
        for target in DEFAULT_TARGETS:
            self.write(target)

    def write(self, target: str, scene: bytes = SCENE) -> None:
        folder = self.root / f"scene-evidence-{target}"
        folder.mkdir(exist_ok=True)
        for workers in (1, 2, 4):
            lane = folder / f"workers-{workers}"
            lane.mkdir(exist_ok=True)
            (lane / "scene.txt").write_bytes(scene)
            metadata = {
                "schema": 2,
                "contract_version": 1,
                "target": target,
                "build_id": BUILD,
                "fixture_sha256": hashlib.sha256(self.fixture.read_bytes()).hexdigest(),
                "map_sha256": "b" * 64,
                "scene_sha256": hashlib.sha256(scene).hexdigest(),
                "workers_requested": workers,
                "executor": "thread-worker-adapter",
                "parallel_stage": "placement-finalization-v1",
                "partitions_completed": workers,
                "placement_count": 7,
                "partition_placement_counts": [sum(i % workers == p for i in range(7)) for p in range(workers)],
                "observed_worker_threads": workers,
            }
            write_lf(lane / "evidence.json", json.dumps(metadata) + "\n")

    def test_all_five_agree(self) -> None:
        self.assertEqual(compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS), hashlib.sha256(SCENE).hexdigest())

    def test_absent_or_duplicate_target_rejected(self) -> None:
        shutil.rmtree(self.root / "scene-evidence-linux-arm64-gcc")
        with self.assertRaisesRegex(ValueError, "missing"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        self.write("linux-arm64-gcc")
        shutil.copytree(self.root / "scene-evidence-linux-arm64-gcc", self.root / "scene-evidence-linux-arm64-gcc-copy")
        with self.assertRaisesRegex(ValueError, "extra"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        with self.assertRaisesRegex(ValueError, "distinct"):
            compare(self.root, self.fixture, BUILD, (DEFAULT_TARGETS[0],) * 2)

    def test_altered_target_and_build_rejected(self) -> None:
        target = DEFAULT_TARGETS[0]
        path = self.root / f"scene-evidence-{target}" / "workers-1" / "evidence.json"
        metadata = json.loads(path.read_text(encoding="utf-8"))
        metadata["target"] = DEFAULT_TARGETS[1]
        write_lf(path, json.dumps(metadata) + "\n")
        with self.assertRaisesRegex(ValueError, "target identity"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        metadata["target"] = target
        metadata["build_id"] = "c" * 40
        write_lf(path, json.dumps(metadata) + "\n")
        with self.assertRaisesRegex(ValueError, "build identity"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_altered_fixture_rejected(self) -> None:
        self.fixture.write_bytes(b"synthetic fixture v2\n")
        with self.assertRaisesRegex(ValueError, "fixture identity"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_fixture_line_endings_do_not_change_identity(self) -> None:
        self.fixture.write_bytes(b"synthetic fixture v1\r\n")
        self.assertEqual(compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS), hashlib.sha256(SCENE).hexdigest())

    def test_altered_map_reference_rejected(self) -> None:
        target = DEFAULT_TARGETS[0]
        self.write(target, SCENE.replace(b"eawr_scene_synthetic.ted", b"different.ted"))
        with self.assertRaisesRegex(ValueError, "canonical map identity"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_scene_mutation_and_cross_target_difference_rejected(self) -> None:
        target = DEFAULT_TARGETS[1]
        path = self.root / f"scene-evidence-{target}" / "workers-1" / "scene.txt"
        path.write_bytes(SCENE + b"changed transform\n")
        with self.assertRaisesRegex(ValueError, "scene hash"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        lane = path.parent
        scene = SCENE + b"changed transform\n"
        path.write_bytes(scene)
        metadata_path = lane / "evidence.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["scene_sha256"] = hashlib.sha256(scene).hexdigest()
        write_lf(metadata_path, json.dumps(metadata) + "\n")
        with self.assertRaisesRegex(ValueError, "cross-worker"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_duplicate_metadata_key_rejected(self) -> None:
        path = self.root / f"scene-evidence-{DEFAULT_TARGETS[0]}" / "workers-1" / "evidence.json"
        raw = path.read_text(encoding="utf-8")
        write_lf(path, raw.replace('"schema": 2', '"schema": 2, "schema": 2'))
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_partial_target_requires_all_three_worker_lanes(self) -> None:
        target = DEFAULT_TARGETS[0]
        for other in DEFAULT_TARGETS[1:]:
            shutil.rmtree(self.root / f"scene-evidence-{other}")
        self.assertEqual(compare(self.root, self.fixture, BUILD, (target,)), hashlib.sha256(SCENE).hexdigest())
        shutil.rmtree(self.root / f"scene-evidence-{target}" / "workers-4")
        with self.assertRaisesRegex(ValueError, "worker lanes mismatch"):
            compare(self.root, self.fixture, BUILD, (target,))

    def test_metadata_statistics_and_types_rejected(self) -> None:
        path = self.root / f"scene-evidence-{DEFAULT_TARGETS[0]}" / "workers-4" / "evidence.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        mutations = (
            ("schema", True), ("contract_version", 2), ("workers_requested", True),
            ("partitions_completed", 3), ("placement_count", 6),
            ("partition_placement_counts", [2, 2, 3, 0]),
            ("partition_placement_counts", [2, 2, 2, True]),
            ("observed_worker_threads", 1), ("executor", "inline"),
            ("parallel_stage", "other"), ("scene_sha256", 123),
        )
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                metadata = dict(original)
                metadata[field] = value
                write_lf(path, json.dumps(metadata) + "\n")
                with self.assertRaises(ValueError):
                    compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_file_set_and_line_endings_rejected(self) -> None:
        lane = self.root / f"scene-evidence-{DEFAULT_TARGETS[0]}" / "workers-2"
        extra = lane / "extra.txt"
        extra.write_bytes(b"unexpected")
        with self.assertRaisesRegex(ValueError, "only"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        extra.unlink()
        scene = lane / "scene.txt"
        scene.write_bytes(b"\xef\xbb\xbf" + SCENE)
        with self.assertRaisesRegex(ValueError, "BOM-free"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)
        scene.write_bytes(SCENE.replace(b"\n", b"\r\n"))
        with self.assertRaisesRegex(ValueError, "LF-only"):
            compare(self.root, self.fixture, BUILD, DEFAULT_TARGETS)

    def test_producer_does_not_publish_failed_worker_set(self) -> None:
        output = Path(self.temp.name) / "published"
        output.mkdir()
        (output / "sentinel").write_bytes(b"previous complete artifact")
        producer = Path(__file__).with_name("produce_scene_evidence.py")
        fixture = Path(__file__).parents[1] / "assets" / "fixtures" / "scene_fixture.py"
        run = subprocess.run([
            sys.executable, str(producer), "--program", sys.executable,
            "--fixture", str(fixture), "--output", str(output),
            "--target", DEFAULT_TARGETS[0], "--build-id", BUILD,
        ], capture_output=True, check=False)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual({p.name for p in output.iterdir()}, {"sentinel"})
        self.assertEqual((output / "sentinel").read_bytes(), b"previous complete artifact")

    def run_mock_producer(self, output: Path, fail_publication: bool) -> list[tuple[Path, Path]]:
        producer = Path(__file__).with_name("produce_scene_evidence.py")
        fixture = Path(__file__).parents[1] / "assets" / "fixtures" / "scene_fixture.py"
        main = runpy.run_path(str(producer))["main"]
        renames = []
        original_rename = Path.rename

        def export(command: list[str], check: bool) -> subprocess.CompletedProcess:
            self.assertTrue(check)
            lane = Path(command[command.index("--output") + 1])
            lane.mkdir()
            (lane / "scene.txt").write_text(lane.name, encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)

        def rename(source: Path, target: Path) -> Path:
            renames.append((source, target))
            if fail_publication and source.name.startswith(".scene-evidence-stage-") and target == output:
                raise OSError("simulated publication failure")
            return original_rename(source, target)

        argv = [str(producer), "--program", sys.executable, "--fixture", str(fixture),
                "--output", str(output), "--target", DEFAULT_TARGETS[0], "--build-id", BUILD]
        with mock.patch.object(sys, "argv", argv), mock.patch("subprocess.run", side_effect=export), \
                mock.patch.object(Path, "rename", rename):
            if fail_publication:
                with self.assertRaisesRegex(OSError, "simulated publication failure"):
                    main()
            else:
                main()
        return renames

    def test_producer_replaces_prior_evidence_with_sibling_staging_and_backup(self) -> None:
        output = Path(self.temp.name) / "published"
        output.mkdir()
        (output / "sentinel").write_bytes(b"previous complete artifact")

        renames = self.run_mock_producer(output, fail_publication=False)

        self.assertEqual({p.name for p in output.iterdir()}, {"workers-1", "workers-2", "workers-4"})
        self.assertEqual((output / "workers-4" / "scene.txt").read_text(encoding="utf-8"), "workers-4")
        backup = next(target for source, target in renames if source == output)
        self.assertEqual(backup.parent.parent, output.parent)
        self.assertTrue(backup.parent.name.startswith(".scene-evidence-backup-"))
        stage = next(source for source, target in renames if target == output)
        self.assertEqual(stage.parent, output.parent)
        self.assertTrue(stage.name.startswith(".scene-evidence-stage-"))
        self.assertFalse(backup.parent.exists())
        self.assertFalse(stage.exists())

    def test_producer_publication_failure_restores_prior_evidence(self) -> None:
        output = Path(self.temp.name) / "published"
        output.mkdir()
        (output / "sentinel").write_bytes(b"previous complete artifact")

        renames = self.run_mock_producer(output, fail_publication=True)

        self.assertEqual({p.name for p in output.iterdir()}, {"sentinel"})
        self.assertEqual((output / "sentinel").read_bytes(), b"previous complete artifact")
        backup = next(target for source, target in renames if source == output)
        self.assertIn((backup, output), renames)
        self.assertFalse(backup.parent.exists())
        self.assertFalse(next(source for source, target in renames if target == output and source != backup).exists())


if __name__ == "__main__":
    unittest.main()
