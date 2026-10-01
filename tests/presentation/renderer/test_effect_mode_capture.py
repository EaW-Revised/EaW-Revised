"""Opt-in graphical runs of the P1-08 viewer effect mode.

Set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE (and the corpus
roots) as described in test_effect_mode.py.
"""

import json
import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from effect_mode_fixtures import (build_proxy_host, _capture_path, DETACH_FRAME,
    EFFECT_LOGICAL_PATH, install_fixture, INVENTORY, PARENT_EFFECT_LOGICAL_PATH,
    PROXY_ANIMATION_LOGICAL_PATH, PROXY_EFFECT_LOGICAL_PATH, PROXY_HOST_LOGICAL_PATH,
    RELEASE_EFFECT_LOGICAL_PATH, _run, VISIBILITY_ANIMATION_LOGICAL_PATH, VISIBILITY_ATTACH)  # noqa: E402


class EffectModeGraphical(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_unicode_capture_basename_preserves_detach_suffixes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-effect-caf\u00e9-") as temporary:
            directory = pathlib.Path(temporary)
            root = install_fixture(directory)
            capture = directory / "\u6218\u6597.png"
            result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, directory / "effect.json", extra=(
                "--eawr-effect-detach-frame", str(DETACH_FRAME), "--eawr-capture", str(capture)))
            self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
            self.assertEqual(result["status"], "effect_render_passed")
            for label in ("pre-detach", "draining"):
                image = capture.with_name(f"{capture.stem}-{label}{capture.suffix}")
                self.assertTrue(image.is_file(), label)
                self.assertGreater(image.stat().st_size, 0)
            self.assertTrue(capture.is_file())
            self.assertTrue(result["evidence"]["verified"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_synthetic_effect_is_deterministic_and_drawn_where_it_is(self):
        results = []
        with tempfile.TemporaryDirectory(prefix="eawr-effect-mode-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"effect-{run}.json"
                result, completed = _run(root, EFFECT_LOGICAL_PATH, report)
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                results.append(result)

        first, second = results
        self.assertEqual(first["status"], "effect_render_passed")
        self.assertEqual(first["run"]["frames"], 60)
        self.assertFalse(first["run"]["simulation_snapshot_submitted"])
        self.assertEqual(first["effect"]["emitters"], 5)
        emitters = {entry["name"]: entry for entry in first["emitters"]}
        self.assertEqual(emitters["billboard-additive"]["adapter"], "eawr-particle-billboard-v1")
        self.assertEqual(emitters["billboard-additive"]["blend"], "additive")
        self.assertEqual(emitters["xy-alpha"]["adapter"], "eawr-particle-xy-aligned-v1")
        self.assertEqual(emitters["xy-alpha"]["blend"], "alpha")
        self.assertEqual(emitters["kite-additive"]["adapter"], "eawr-particle-kite-v1")
        self.assertEqual(emitters["heat"]["adapter"], "eawr-particle-heat-v1")
        self.assertEqual(emitters["heat"]["phase"], "heat")
        self.assertFalse(emitters["depth-sprite"]["drawn"])
        self.assertIn("depth sprite", emitters["depth-sprite"]["cause"])
        for entry in first["emitters"]:
            self.assertFalse(entry["legacy_route_supported"])
        self.assertEqual(len(first["frames"]), 60)
        self.assertGreater(first["frames"][-1]["particles"], 0)
        # Without the option nothing is scheduled and nothing is released early.
        self.assertIsNone(first["run"]["detach_frame"])
        self.assertFalse(first["detach"]["requested"])
        self.assertEqual(first["detach"]["captures"], [])
        self.assertEqual(first["lifecycle"]["released_by"], "owner")
        self.assertFalse(first["visibility"]["requested"])
        self.assertEqual(first["visibility"]["frames"], [])
        self.assertTrue(all(frame["advanced"] and not frame["detached"] and not frame["finished"]
                            for frame in first["frames"]))
        self.assertTrue(first["determinism"]["dry_run_matches"])
        self.assertEqual(first["lifecycle"]["live_rids_after_release"], 0)
        self.assertGreater(first["lifecycle"]["live_rids_before_release"], 0)
        self.assertEqual(first["lifecycle"]["registry_resources_after_release"], 0)

        evidence = first["evidence"]
        self.assertTrue(evidence["verified"])
        self.assertEqual(evidence["check"], "projected_emitter_bounds")
        self.assertGreaterEqual(evidence["inside_coverage"], 0.01)
        self.assertLessEqual(evidence["outside_coverage"], 0.005)

        # Fixed seed, fixed step, fixed camera: the second run is identical.
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["determinism"]["final_stream_hash"],
                         second["determinism"]["final_stream_hash"])
        self.assertEqual(evidence["capture_sha256"], second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_synthetic_parent_lifecycle_is_drawn_and_deterministic(self):
        results = []
        with tempfile.TemporaryDirectory(prefix="eawr-parent-effect-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"parent-{run}.json"
                result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, report)
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                results.append(result)
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed")
        self.assertTrue(first["determinism"]["dry_run_matches"])
        self.assertTrue(first["evidence"]["verified"])
        self.assertEqual([entry["renderer_id"] for entry in first["emitters"]], [22, 22, 22])
        self.assertEqual([entry["parent_emitter"] for entry in first["emitters"]], [None, 0, 0])
        self.assertGreater(sum(frame["child_instances_started"] for frame in first["frames"]), 0)
        self.assertGreater(sum(frame["child_instances_detached"] for frame in first["frames"]), 0)
        self.assertGreater(sum(frame["death_bursts"] for frame in first["frames"]), 0)
        self.assertGreater(first["frames"][-1]["emitter_particles"][2], 0)
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["evidence"]["capture_sha256"], second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_detach_drains_residuals_then_releases_once(self):
        results = []
        with tempfile.TemporaryDirectory(prefix="eawr-effect-drain-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"drain-{run}.json"
                capture = _capture_path(f"drain-{run}", pathlib.Path(temporary))
                result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, report, extra=(
                    "--eawr-effect-detach-frame", str(DETACH_FRAME), "--eawr-capture", str(capture)))
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                for label in ("pre-detach", "draining"):
                    self.assertTrue(capture.with_name(f"{capture.stem}-{label}.png").is_file(), label)
                results.append(result)
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed")
        self.assertEqual(first["run"]["detach_frame"], DETACH_FRAME)
        detach = first["detach"]
        self.assertTrue(detach["requested"])
        self.assertTrue(detach["leave_particles"])
        self.assertEqual(detach["result"], "draining")
        self.assertEqual(detach["replay_result"], "draining")
        self.assertTrue(detach["rendered_before_detach"])
        self.assertTrue(first["determinism"]["dry_run_matches"])
        frames = first["frames"]
        self.assertEqual(len(frames), 60)
        self.assertEqual([frame["index"] for frame in frames], list(range(60)))
        self.assertFalse(any(frame["detached"] for frame in frames[:DETACH_FRAME]))
        self.assertTrue(all(frame["detached"] for frame in frames[DETACH_FRAME:]))
        # Residuals are drawn right after the detach, and the parent's own
        # children still act: birth trail while it lives, one death burst.
        self.assertGreater(frames[DETACH_FRAME]["quads"], 0)
        self.assertEqual(frames[DETACH_FRAME]["emitter_particles"][0], 1)
        after = frames[DETACH_FRAME:]
        self.assertGreater(sum(frame["spawned"] for frame in after), 0)
        self.assertEqual(sum(frame["death_bursts"] for frame in after), 1)
        self.assertEqual(sum(frame["child_instances_started"] for frame in after), 0)
        self.assertEqual(max(frame["emitter_particles"][0] for frame in after if frame["advanced"]), 1)
        finished = [frame["index"] for frame in frames if frame["finished"]]
        self.assertTrue(finished)
        release = finished[0]
        self.assertEqual(detach["release_frame"], release)
        self.assertEqual(detach["released_by"], "completion")
        self.assertEqual(first["lifecycle"]["released_by"], "completion")
        self.assertTrue(all(frame["particles"] > 0 for frame in frames[DETACH_FRAME:release]))
        self.assertEqual(frames[release]["particles"], 0)
        self.assertTrue(frames[release]["advanced"])
        self.assertTrue(all(not frame["advanced"] and frame["released"] for frame in frames[release + 1:]))
        self.assertTrue(all(frame["resources"] > 0 for frame in frames[:release]))
        self.assertTrue(all(frame["resources"] == 0 for frame in frames[release:]))
        self.assertGreater(first["lifecycle"]["live_rids_before_release"], 0)
        self.assertEqual(first["lifecycle"]["live_rids_after_release"], 0)
        self.assertEqual(first["lifecycle"]["registry_resources_after_release"], 0)
        # Decoded pixels: the pre-detach and draining captures are drawn inside
        # their projected bounds; the final capture is empty, and that is a
        # verified lifecycle outcome rather than a never-rendered failure.
        captures = {entry["label"]: entry for entry in detach["captures"]}
        self.assertEqual(sorted(captures), ["draining", "pre-detach"])
        self.assertEqual(captures["pre-detach"]["index"], DETACH_FRAME - 1)
        self.assertTrue(DETACH_FRAME < captures["draining"]["index"] < release)
        for entry in captures.values():
            self.assertTrue(entry["verified"], entry)
            self.assertTrue(entry["written"])
            self.assertEqual(entry["check"], "projected_emitter_bounds")
            self.assertGreaterEqual(entry["inside_coverage"], 0.01)
            self.assertLessEqual(entry["outside_coverage"], 0.005)
        self.assertTrue(detach["final_capture_empty"])
        self.assertEqual(first["evidence"]["check"], "empty_after_detach")
        self.assertTrue(first["evidence"]["verified"])
        self.assertEqual(first["evidence"]["drawn_samples"], 0)
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["detach"], second["detach"])
        self.assertEqual(first["evidence"]["capture_sha256"], second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_detach_without_leave_particles_releases_immediately(self):
        results = []
        with tempfile.TemporaryDirectory(prefix="eawr-effect-release-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"release-{run}.json"
                capture = _capture_path(f"release-{run}", pathlib.Path(temporary))
                result, completed = _run(root, RELEASE_EFFECT_LOGICAL_PATH, report, extra=(
                    "--eawr-effect-detach-frame", str(DETACH_FRAME), "--eawr-capture", str(capture)))
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                results.append(result)
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed")
        detach = first["detach"]
        self.assertFalse(detach["leave_particles"])
        self.assertEqual(detach["result"], "released")
        self.assertEqual(detach["release_frame"], DETACH_FRAME)
        self.assertEqual(detach["released_by"], "detach")
        frames = first["frames"]
        self.assertTrue(all(frame["advanced"] for frame in frames[:DETACH_FRAME]))
        self.assertGreater(frames[DETACH_FRAME - 1]["quads"], 0)
        self.assertGreater(frames[DETACH_FRAME - 1]["resources"], 0)
        for frame in frames[DETACH_FRAME:]:
            self.assertFalse(frame["advanced"])
            self.assertTrue(frame["released"])
            self.assertEqual((frame["particles"], frame["quads"], frame["resources"]), (0, 0, 0))
        self.assertGreater(first["lifecycle"]["live_rids_before_release"], 0)
        self.assertEqual(first["lifecycle"]["live_rids_after_release"], 0)
        self.assertEqual(first["lifecycle"]["registry_resources_after_release"], 0)
        captures = {entry["label"]: entry for entry in detach["captures"]}
        self.assertEqual(sorted(captures), ["pre-detach", "released"])
        self.assertEqual(captures["pre-detach"]["check"], "projected_emitter_bounds")
        self.assertGreaterEqual(captures["pre-detach"]["inside_coverage"], 0.01)
        self.assertEqual(captures["released"]["check"], "empty_after_detach")
        self.assertEqual(captures["released"]["drawn_samples"], 0)
        for entry in captures.values():
            self.assertTrue(entry["verified"], entry)
        self.assertEqual(first["evidence"]["check"], "empty_after_detach")
        self.assertTrue(first["evidence"]["verified"])
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["evidence"]["capture_sha256"], second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_detach_frame_option_errors(self):
        with tempfile.TemporaryDirectory(prefix="eawr-effect-detach-errors-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for index, value in enumerate(("60", "-1", "six")):
                with self.subTest(value=value):
                    report = pathlib.Path(temporary) / f"detach-error-{index}.json"
                    result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, report,
                                             extra=("--eawr-effect-detach-frame", value))
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(result["status"], "failed")
                    self.assertIn("--eawr-effect-detach-frame", result["failure"])
            # A trailing flag with no value is an error, not a silent no-detach run.
            with self.subTest(value=None):
                report = pathlib.Path(temporary) / "detach-error-trailing.json"
                result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, report,
                                         extra=("--eawr-effect-detach-frame",))
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(result["status"], "failed")
                self.assertIn("--eawr-effect-detach-frame", result["failure"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_visibility_hide_drains_and_reappearance_respawns(self):
        results = self._visibility_runs("respawn", PARENT_EFFECT_LOGICAL_PATH, "visibility-respawn")
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed", first["failure"])
        visibility = first["visibility"]
        self.assertTrue(visibility["requested"])
        self.assertEqual(visibility["policy"], "respawn")
        self.assertEqual((visibility["bone"], visibility["bone_index"]), ("socket", 2))
        self.assertTrue(visibility["leave_particles"])
        self.assertTrue(visibility["replay_matches"])
        self.assertTrue(first["determinism"]["dry_run_matches"])
        events = visibility["frames"]
        self.assertEqual(len(events), 90)
        hides, shows = self._edges(events)
        self.assertEqual(len(hides), 2)
        self.assertEqual(len(shows), 1)
        # Exactly one detach per visible-to-hidden edge, and only there.
        for index, entry in enumerate(events):
            self.assertEqual(entry["detached"], "draining" if index in hides else "", index)
            self.assertEqual(entry["spawned"], index == 0 or index in shows, index)
            self.assertEqual(entry["active"], entry["visible"], index)
        self.assertEqual((visibility["generations"], visibility["detaches"]), (2, 2))
        # The first generation's drain overlaps the reappeared generation and
        # finishes; the second is still draining at the end of the run.
        self.assertGreaterEqual(visibility["drains_released"], 1)
        self.assertEqual(visibility["drains_cut_short"], 0)
        self.assertEqual(max(entry["live_instances"] for entry in events), 2)
        self.assertEqual(visibility["released_at_end"], 1)
        self.assertEqual(first["lifecycle"]["released_by"], "end_of_run")
        frames = first["frames"]
        self.assertTrue(all(frames[index]["quads"] > 0 for index in (hides[0] - 1, hides[0])))
        # A parent alive at the hide still dies once and bursts once; none is born after it.
        self.assertLessEqual(sum(frame["death_bursts"] for frame in frames[hides[0]:shows[0]]), 1)
        self.assertEqual(sum(frame["child_instances_started"] for frame in frames[hides[0]:shows[0]]), 0)
        # The host moves while the lifecycle runs.
        origins = first["attachment"]["origins"]
        self.assertEqual(len(origins), 90)
        self.assertGreater(origins[-1][0], origins[0][0] + 3)
        self._assert_clean(first)
        captures = {entry["label"]: entry for entry in visibility["captures"]}
        self.assertEqual(sorted(captures), ["hidden-draining", "pre-hide", "reappeared"])
        self.assertEqual(captures["pre-hide"]["index"], hides[0] - 1)
        self.assertTrue(hides[0] <= captures["hidden-draining"]["index"] < shows[0])
        self.assertTrue(shows[0] <= captures["reappeared"]["index"] < hides[1])
        for entry in captures.values():
            self.assertTrue(entry["verified"], entry)
            self.assertEqual(entry["check"], "projected_emitter_bounds")
        self.assertEqual(first["detach"]["captures"], [])
        self.assertTrue(first["evidence"]["verified"])
        self.assertEqual(first["evidence"]["check"], "projected_emitter_bounds")
        self._assert_repeat(first, second)

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_visibility_hide_without_leave_particles_releases(self):
        results = self._visibility_runs("respawn", RELEASE_EFFECT_LOGICAL_PATH, "visibility-release")
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed", first["failure"])
        visibility = first["visibility"]
        self.assertFalse(visibility["leave_particles"])
        events = visibility["frames"]
        hides, shows = self._edges(events)
        self.assertEqual((len(hides), len(shows)), (2, 1))
        for index, entry in enumerate(events):
            self.assertEqual(entry["detached"], "released" if index in hides else "", index)
            # Nothing drains: a hidden host has no live instance at all.
            self.assertEqual(entry["live_instances"], 1 if entry["visible"] else 0, index)
            if not entry["visible"]:
                frame = first["frames"][index]
                self.assertEqual((frame["advanced"], frame["particles"], frame["quads"], frame["resources"]),
                                 (False, 0, 0, 0), index)
        self.assertEqual((visibility["generations"], visibility["detaches"], visibility["drains_released"]),
                         (2, 2, 0))
        self.assertEqual(visibility["released_at_end"], 0)
        self.assertEqual(first["lifecycle"]["released_by"], "visibility")
        self._assert_clean(first)
        captures = {entry["label"]: entry for entry in visibility["captures"]}
        self.assertEqual(sorted(captures), ["hidden-released", "pre-hide", "reappeared"])
        self.assertEqual(captures["hidden-released"]["check"], "empty_after_detach")
        self.assertEqual(captures["hidden-released"]["drawn_samples"], 0)
        for entry in captures.values():
            self.assertTrue(entry["verified"], entry)
        self.assertEqual(first["evidence"]["check"], "empty_after_detach")
        self.assertTrue(first["evidence"]["verified"])
        self._assert_repeat(first, second)

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_visibility_stay_detached_ignores_reappearance(self):
        results = self._visibility_runs("stay-detached", PARENT_EFFECT_LOGICAL_PATH, "visibility-stay")
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed", first["failure"])
        visibility = first["visibility"]
        self.assertEqual(visibility["policy"], "stay_detached")
        events = visibility["frames"]
        hides, shows = self._edges(events)
        self.assertEqual(len(shows), 1)
        self.assertEqual([index for index, entry in enumerate(events) if entry["spawned"]], [0])
        self.assertEqual([index for index, entry in enumerate(events) if entry["detached"]], [hides[0]])
        self.assertFalse(any(entry["active"] for entry in events[hides[0]:]))
        self.assertEqual((visibility["generations"], visibility["detaches"], visibility["drains_released"]),
                         (1, 1, 1))
        self.assertEqual(events[-1]["live_instances"], 0)
        self.assertEqual(first["lifecycle"]["released_by"], "visibility")
        self._assert_clean(first)
        self.assertEqual(first["evidence"]["check"], "empty_after_detach")
        self.assertTrue(first["evidence"]["verified"])
        self.assertEqual(first["evidence"]["drawn_samples"], 0)
        self._assert_repeat(first, second)

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_visibility_option_errors(self):
        attach = ("--eawr-effect-attach", VISIBILITY_ATTACH,
                  "--eawr-animation", VISIBILITY_ANIMATION_LOGICAL_PATH)
        cases = (
            (attach + ("--eawr-effect-visibility", "sometimes"), "expects respawn or stay-detached"),
            (attach + ("--eawr-effect-visibility",), "expects respawn or stay-detached"),
            (attach + ("--eawr-effect-visibility", "respawn", "--eawr-effect-detach-frame", "6"),
             "cannot be combined"),
            (("--eawr-effect-visibility", "respawn"), "requires --eawr-effect-attach"),
        )
        with tempfile.TemporaryDirectory(prefix="eawr-visibility-errors-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for index, (extra, expected) in enumerate(cases):
                with self.subTest(case=index):
                    report = pathlib.Path(temporary) / f"visibility-error-{index}.json"
                    result, completed = _run(root, PARENT_EFFECT_LOGICAL_PATH, report, extra=extra)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(result["status"], "failed")
                    self.assertIn(expected, result["failure"])

    def _visibility_runs(self, policy: str, effect: str, name: str) -> list:
        results = []
        with tempfile.TemporaryDirectory(prefix=f"eawr-{name}-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"{name}-{run}.json"
                capture = _capture_path(f"{name}-{run}", pathlib.Path(temporary))
                result, completed = _run(root, effect, report, extra=(
                    "--eawr-effect-attach", VISIBILITY_ATTACH,
                    "--eawr-animation", VISIBILITY_ANIMATION_LOGICAL_PATH,
                    "--eawr-effect-visibility", policy, "--eawr-effect-frames", "90",
                    "--eawr-capture", str(capture)))
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                self.assertNotIn("leaked", completed.stdout.lower())
                results.append(result)
        return results

    @staticmethod
    def _edges(events: list) -> tuple[list, list]:
        hides = [index for index in range(1, len(events))
                 if events[index - 1]["visible"] and not events[index]["visible"]]
        shows = [index for index in range(1, len(events))
                 if not events[index - 1]["visible"] and events[index]["visible"]]
        return hides, shows

    def _assert_clean(self, result: dict):
        self.assertGreater(result["visibility"]["peak_live_rids"], 0)
        self.assertEqual(result["lifecycle"]["live_rids_after_release"], 0)
        self.assertEqual(result["lifecycle"]["registry_resources_after_release"], 0)
        self.assertEqual(result["frames"][-1]["resources"] == 0, result["visibility"]["released_at_end"] == 0)

    def _assert_repeat(self, first: dict, second: dict):
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["visibility"], second["visibility"])
        self.assertEqual(first["evidence"]["capture_sha256"], second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_missing_attachment_is_a_diagnostic(self):
        with tempfile.TemporaryDirectory(prefix="eawr-effect-attach-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "attach.json"
            result, completed = _run(root, EFFECT_LOGICAL_PATH, report,
                                     extra=("--eawr-effect-attach", "data/art/models/absent.alo:socket"))
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(result["failure"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_proxy_mesh_modes_animate_and_repeat(self):
        results = []
        with tempfile.TemporaryDirectory(prefix="eawr-proxy-mesh-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            for run in range(2):
                report = pathlib.Path(temporary) / f"proxy-{run}.json"
                extra = ("--eawr-effect-proxy-host", PROXY_HOST_LOGICAL_PATH,
                         "--eawr-effect-proxy", "spark",
                         "--eawr-animation", PROXY_ANIMATION_LOGICAL_PATH,
                         "--eawr-effect-frames", "30", "--eawr-effect-capacity", "512")
                result, completed = _run(root, PROXY_EFFECT_LOGICAL_PATH, report, extra=extra)
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                self.assertNotIn("leaked", completed.stdout.lower())
                results.append(result)
        first, second = results
        self.assertEqual(first["status"], "effect_render_passed")
        self.assertEqual([entry["creator_id"] for entry in first["emitters"]], [35, 35, 35])
        self.assertEqual([entry["mesh_mode"] for entry in first["emitters"]], [1, 2, 3])
        self.assertEqual(first["proxy_mesh"]["owner_mesh_index"], 1)
        self.assertEqual(first["proxy_mesh"]["name"], "spark")
        origins = first["proxy_mesh"]["mesh_origins"]
        self.assertEqual(len(origins), 30)
        self.assertGreater(origins[-1][0], origins[0][0] + 3)
        self.assertEqual(first["proxy_mesh"]["proxy_origins"][0][2], origins[0][2] + 2)
        self.assertFalse(first["attachment"]["requested"])
        self.assertTrue(first["determinism"]["dry_run_matches"])
        self.assertTrue(first["evidence"]["verified"])
        self.assertGreater(first["evidence"]["inside_coverage"], 0.01)
        self.assertLessEqual(first["evidence"]["outside_coverage"], 0.005)
        self.assertGreater(first["frames"][8]["particles"], first["frames"][8]["spawned"])
        self.assertEqual(first["lifecycle"]["live_rids_after_release"], 0)
        self.assertEqual(first["lifecycle"]["registry_resources_after_release"], 0)
        self.assertEqual(first["frames"], second["frames"])
        self.assertEqual(first["determinism"]["final_stream_hash"],
                         second["determinism"]["final_stream_hash"])
        self.assertEqual(first["evidence"]["capture_sha256"],
                         second["evidence"]["capture_sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical effect mode")
    def test_proxy_selector_errors_before_rendering(self):
        with tempfile.TemporaryDirectory(prefix="eawr-proxy-errors-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            cases = (
                (("--eawr-effect-proxy-host", PROXY_HOST_LOGICAL_PATH,
                  "--eawr-effect-proxy", "missing"), "absent"),
                (("--eawr-effect-proxy-host", PROXY_HOST_LOGICAL_PATH,
                  "--eawr-effect-proxy", "spark",
                  "--eawr-effect-attach", PROXY_HOST_LOGICAL_PATH + ":socket"), "either"),
                (("--eawr-effect-proxy-host", PROXY_HOST_LOGICAL_PATH,), "both"),
            )
            for index, (extra, expected) in enumerate(cases):
                with self.subTest(case=index):
                    report = pathlib.Path(temporary) / f"error-{index}.json"
                    result, completed = _run(root, PROXY_EFFECT_LOGICAL_PATH, report, extra=extra)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn(expected, result["failure"])
            host = root / "GameData/Data/Art/Models/P_Synthetic_Proxy_Host.ALO"
            host.write_bytes(build_proxy_host(duplicate_proxy=True))
            report = pathlib.Path(temporary) / "ambiguous.json"
            result, completed = _run(root, PROXY_EFFECT_LOGICAL_PATH, report,
                                     extra=("--eawr-effect-proxy-host", PROXY_HOST_LOGICAL_PATH,
                                            "--eawr-effect-proxy", "spark"))
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("ambiguous", result["failure"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus runs")
    def test_installed_corpus_effect_per_implemented_family(self):
        document = json.loads(INVENTORY.read_text(encoding="utf-8"))
        game_root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        attachment = dict(document["attachment_run"], family="attachment",
                          adapter="eawr-particle-billboard-v1")
        for sample in document["corpus_runs"] + [attachment]:
            mod_root = os.environ.get("EAWR_REMAKE_MOD_ROOT") if sample["profile"] == "remake" else None
            if sample["profile"] == "remake" and not mod_root:
                continue
            with self.subTest(family=sample["family"]), \
                    tempfile.TemporaryDirectory(prefix="eawr-effect-corpus-") as temporary:
                report = pathlib.Path(temporary) / "corpus.json"
                extra = ()
                if sample.get("attach"):
                    extra = ("--eawr-effect-attach", sample["attach"])
                if sample.get("animation_logical_path"):
                    extra += ("--eawr-animation", sample["animation_logical_path"])
                result, completed = _run(game_root, sample["logical_path"], report, mod_root, extra)
                self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                self.assertEqual(result["status"], "effect_render_passed")
                self.assertEqual(result["effect"]["sha256"], sample["sha256"])
                self.assertIn(sample["adapter"], result["adapters_used"])
                self.assertEqual(result["determinism"]["final_stream_hash"], sample["final_stream_hash"])
                self.assertTrue(result["determinism"]["dry_run_matches"])
                self.assertEqual(result["lifecycle"]["live_rids_after_release"], 0)



if __name__ == "__main__":
    unittest.main()
