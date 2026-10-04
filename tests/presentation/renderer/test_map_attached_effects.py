"""Synthetic CPU and optional graphical contracts for land MapMode attachments."""

import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import map_attached_fixture as fixture  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402


class AttachedFixtureContract(unittest.TestCase):
    def test_dedicated_fixture_has_named_socket_and_source_order(self):
        host = fixture.host_alo()
        ted = fixture.ted_bytes()
        self.assertEqual(struct.unpack_from("<I", host)[0], 0x200)
        self.assertEqual(struct.unpack_from("<I", ted)[0], 0)
        self.assertIn(b"NamedSocket\0", host)
        socket = struct.pack("<iI12f", 0, 1,
                             1.0, 0.0, 0.0, 4.0,
                             0.0, 1.0, 0.0, 2.0,
                             0.0, 0.0, 1.0, 40.0)
        self.assertIn(socket, host)
        self.assertEqual(len(fixture.PROXIES), 6)
        self.assertEqual(len({name for name, _, _ in fixture.PROXIES}), 6)

    def test_idle_clip_scripts_named_socket_visibility(self):
        clip = fixture.idle_clip()
        self.assertEqual(struct.unpack_from("<I", clip)[0], 0x1000)
        self.assertIn(b"NamedSocket\0", clip)
        frames = fixture.IDLE_CLIP_FRAMES
        bits = clip[-((frames + 7) // 8):]
        visible = [bool(bits[index // 8] & (1 << (index % 8))) for index in range(frames)]
        self.assertEqual([index for index, shown in enumerate(visible) if not shown],
                         list(fixture.IDLE_HIDDEN_FRAMES))
        # At samples of 1/30 s the 10 fps clip hides on 18-41 and 72-89.
        hidden = [n for n in range(90) if not visible[(n // 3) % (frames - 1)]]
        self.assertEqual(hidden, list(range(18, 42)) + list(range(72, 90)))

    def test_idle_owner_is_opt_in_and_owned_by_map_mode(self):
        source = mode_source("map_mode")
        physical = (ROOT / "apps/viewer/src/map_mode_particles.cpp").read_text(encoding="utf-8")
        self.assertIn('"--eawr-map-effect-animation"', source)
        self.assertIn("EffectAnimation effect_animation{EffectAnimation::none}",
                      (ROOT / "apps/viewer/src/map_mode.hpp").read_text(encoding="utf-8"))
        self.assertIn("particles->has_work()", source)
        # With owners the effects-off comparison is always captured, and an
        # owner's system is probed before any generation exists.
        self.assertIn("state.particles->has_live() || state.particles->has_owners()", source)
        self.assertIn("OwnerProbeBackend probe(", source)
        self.assertIn("particles::map_owner_sample(*clip.player, sample)", source)
        # Owners release before any static handle and before the registry.
        release = (ROOT / "apps/viewer/src/map_mode_particles.cpp").read_text(encoding="utf-8")
        release = release[release.index("void MapParticleProvider::release()"):]
        self.assertLess(release.index("owned.owner.release_all()"),
                        release.index("registry_->release(placement.handle)"))

@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST for graphical MapMode checks")
class AttachedGraphicalContract(unittest.TestCase):
    def run_map(self, root, report, *args):
        executable = os.environ["EAWR_GODOT_EXECUTABLE"]
        completed = subprocess.run(
            [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", fixture.MAP_LOGICAL_PATH,
             "--eawr-game-root", str(root), "--eawr-report", str(report),
             "--eawr-populate", "--eawr-map-particle-capacity", "256", *args],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=120, check=False)
        self.assertTrue(report.is_file(), completed.stdout)
        result = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
        self.assertEqual(result["status"], "map_render_passed", result.get("failure"))
        return result

    def test_admission_pixels_determinism_and_teardown(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory)
            selected = ("--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")
            first = self.run_map(root, directory / "first.json", *selected)
            repeat = self.run_map(root, directory / "repeat.json", *selected)
            off = self.run_map(root, directory / "off.json", *selected,
                               "--eawr-map-effects", "off")
            plan = first["map_attached_effects"]
            records = plan["records"]
            self.assertEqual(len(records), 6)
            self.assertEqual([entry["status"] for entry in records],
                             ["admitted", "hidden", "hidden", "hidden", "unresolved", "unsupported"])
            self.assertEqual([entry["cause"] for entry in records],
                             ["none", "alt_mismatch", "lod_mismatch", "hidden_proxy",
                              "unresolved_reference", "capacity_exhausted"])
            self.assertEqual(plan["allocated_capacity"], 256)
            self.assertEqual(records[0]["emitter_origin"], list(fixture.EXPECTED_EMITTER_ORIGIN))
            self.assertEqual(records[0]["model_logical_path"],
                             "data/art/models/eawr_attached_host.alo")
            self.assertEqual(records[0]["runtime_status"], "drawn")
            self.assertGreater(records[0]["changed_pixels"], 0)
            self.assertTrue(all(entry["runtime_status"] == "not_spawned" for entry in records[1:]))
            self.assertEqual(records[0]["seed"], repeat["map_attached_effects"]["records"][0]["seed"])
            self.assertEqual(records[0]["stream_hash"], repeat["map_attached_effects"]["records"][0]["stream_hash"])
            self.assertEqual(first["evidence"]["capture_sha256"], repeat["evidence"]["capture_sha256"])
            self.assertNotEqual(first["evidence"]["capture_sha256"], off["evidence"]["capture_sha256"])
            self.assertEqual(off["map_attached_effects"]["records"][0]["runtime_status"], "omitted_by_request")
            particles = first["map_particles"]
            self.assertEqual(particles["placements"][0]["scale_raw"], 3 * (1 << 23))
            self.assertTrue(particles["evidence_verified"])
            self.assertGreater(particles["live_rids_at_capture"], 0)
            self.assertGreater(particles["live_resources_at_capture"], 0)
            self.assertEqual(particles["live_rids_after_release"], 0)
            self.assertEqual(particles["live_resources_after_release"], 0)
            self.assertEqual(particles["fog_consumers_after_release"], 0)

    def test_static_effect_waiting_for_first_particle_is_accounted_empty(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-delay-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, effect_start_delay=3.0)
            result = self.run_map(root, directory / "delay.json",
                                  "--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")
            placement = result["map_particles"]["placements"][0]
            self.assertEqual(placement["status"], "live_no_particles")
            self.assertEqual(placement["particles"], 0)
            self.assertEqual(result["map_particles"]["empty_placements_at_capture"], 1)
            self.assertTrue(result["map_particles"]["evidence_verified"])

    def test_fogged_static_effect_waiting_for_first_particle_is_accounted_empty(self):
        from test_map_fog import write_grid

        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-fog-delay-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, effect_start_delay=3.0)
            grid = directory / "bright.grid"
            digest = write_grid(grid, 2, bytes([255] * 6),
                                origin=(100, 100), cell=(200, 180))
            result = self.run_map(
                root, directory / "delay-fog.json", "--eawr-map-effect-alt", "0",
                "--eawr-map-effect-lod", "0", "--eawr-fog-grid", str(grid),
                "--eawr-fog-sha256", digest, "--eawr-fog-team", "2",
                "--eawr-fog-revision", "1", "--eawr-fog-tick", "1")
            particles = result["map_particles"]
            placement = particles["placements"][0]
            self.assertEqual(placement["status"], "live_no_particles")
            self.assertEqual(placement["particles"], 0)
            self.assertEqual(particles["empty_placements_at_capture"], 1)
            self.assertTrue(particles["fog_bound_at_capture"])
            self.assertEqual(particles["fog_consumers_at_capture"], 1)
            self.assertTrue(particles["evidence_verified"])

    def test_missing_selection_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-selector-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory)
            result = self.run_map(root, directory / "missing.json")
            records = result["map_attached_effects"]["records"]
            self.assertEqual(records[0]["cause"], "missing_alt_selection")
            self.assertEqual(records[1]["cause"], "missing_alt_selection")
            self.assertEqual(records[2]["cause"], "missing_alt_selection")
            self.assertEqual(records[0]["runtime_status"], "not_spawned")

    def test_fog_registration_returns_to_baseline(self):
        from test_map_fog import write_grid

        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-fog-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory)
            grid = directory / "bright.grid"
            digest = write_grid(grid, 2, bytes([255] * 6),
                                origin=(100, 100), cell=(200, 180))
            result = self.run_map(
                root, directory / "fog.json", "--eawr-map-effect-alt", "0",
                "--eawr-map-effect-lod", "0", "--eawr-fog-grid", str(grid),
                "--eawr-fog-sha256", digest, "--eawr-fog-team", "2",
                "--eawr-fog-revision", "1", "--eawr-fog-tick", "1")
            particles = result["map_particles"]
            self.assertTrue(particles["fog_bound_at_capture"])
            self.assertEqual(particles["fog_consumers_at_capture"], 1)
            self.assertEqual(particles["fog_consumers_after_release"], 0)
            self.assertEqual(particles["live_rids_after_release"], 0)
            self.assertEqual(particles["live_resources_after_release"], 0)

    def test_enhanced_mesh_attached_path_binds_parent_mesh(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-mesh-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, mesh_effect=True)
            selected = ("--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")
            first = self.run_map(root, directory / "first.json", *selected)
            repeat = self.run_map(root, directory / "repeat.json", *selected)
            attached = first["map_attached_effects"]["records"][0]
            self.assertEqual(attached["runtime_status"], "drawn")
            self.assertGreater(attached["changed_pixels"], 0)
            self.assertEqual(attached["stream_hash"],
                             repeat["map_attached_effects"]["records"][0]["stream_hash"])
            self.assertEqual(first["map_particles"]["placements"][0]["status"], "drawn")
            self.assertEqual(first["map_particles"]["live_rids_after_release"], 0)

    def test_enhanced_mesh_without_owner_mesh_is_listed_and_skipped(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-no-mesh-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, mesh_effect=True, host_mesh=False)
            result = self.run_map(root, directory / "skipped.json",
                                  "--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")
            placement = result["map_particles"]["placements"][0]
            self.assertEqual(placement["status"], "skipped")
            self.assertEqual(len(placement["causes"]), 3)
            self.assertTrue(all("mesh binding" in cause for cause in placement["causes"]))
            self.assertEqual(result["map_attached_effects"]["records"][0]["runtime_causes"],
                             placement["causes"])
            self.assertEqual(result["map_particles"]["live_rids_after_release"], 0)

    def test_unbindable_mesh_emitters_leave_ordinary_emitter_running(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-attached-mixed-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, mixed_effect=True, host_mesh=False)
            result = self.run_map(root, directory / "mixed.json",
                                  "--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")
            placement = result["map_particles"]["placements"][0]
            self.assertEqual(placement["status"], "drawn")
            self.assertGreater(placement["changed_pixels"], 0)
            self.assertEqual(len(placement["causes"]), 3)
            self.assertTrue(all("skipped: mesh binding" in cause for cause in placement["causes"]))
            self.assertEqual(len(placement["emitters"]), 1)
            self.assertEqual(result["map_particles"]["live_rids_after_release"], 0)


IDLE = ("--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0",
        "--eawr-map-particle-capacity", "512", "--eawr-map-attached-capacity", "128")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST for graphical MapMode checks")
class IdleOwnerGraphicalContract(unittest.TestCase):
    def launch(self, root, report, *args, populate=True, map_path=fixture.MAP_LOGICAL_PATH):
        executable = os.environ["EAWR_GODOT_EXECUTABLE"]
        completed = subprocess.run(
            [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", map_path,
             "--eawr-game-root", str(root), "--eawr-report", str(report),
             *(("--eawr-populate",) if populate else ()), *args],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=120, check=False)
        self.assertTrue(report.is_file(), completed.stdout)
        return completed.returncode, json.loads(report.read_text(encoding="utf-8"))

    def run_passing(self, root, report, *args):
        code, result = self.launch(root, report, *args)
        self.assertEqual(code, 0, result.get("failure"))
        self.assertEqual(result["status"], "map_render_passed", result.get("failure"))
        return result

    def test_idle_owner_updates_mesh_frame(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-mesh-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True, mesh_effect=True)
            result = self.run_passing(root, directory / "mesh.json", *IDLE,
                                      "--eawr-map-effect-animation", "idle")
            records = result["map_attached_effects"]["records"]
            self.assertEqual(records[0]["owner"]["kind"], "idle_clip")
            self.assertGreater(records[0]["owner"]["samples"], 0)
            self.assertEqual(result["map_particles"]["live_rids_after_release"], 0)

    @staticmethod
    def owner_summary(owner):
        spawns = [event["sample"] for event in owner["events"] if event["spawned_generation"] is not None]
        detaches = [(event["sample"], event["detached"]) for event in owner["events"] if event["detached"]]
        releases = [event["sample"] for event in owner["events"] for _ in range(event["drains_released"])]
        return spawns, detaches, releases

    def test_idle_owner_hides_respawns_drains_and_tears_down(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True)
            first = self.run_passing(root, directory / "first.json", *IDLE,
                                     "--eawr-map-effect-animation", "idle")
            repeat = self.run_passing(root, directory / "repeat.json", *IDLE,
                                      "--eawr-map-effect-animation", "idle")
            plan = first["map_attached_effects"]
            self.assertEqual(plan["effect_animation"], "idle")
            self.assertEqual(plan["mesh_pose"], "bind")
            self.assertEqual(plan["policy"], "respawn")
            self.assertEqual(plan["max_draining"], 1)
            self.assertEqual(plan["animation_failure"], "")
            # Two admitted records of 128 plus one drain each fill 512 exactly.
            self.assertEqual(plan["allocated_capacity"], 256)
            self.assertEqual(plan["drain_headroom"], 256)
            self.assertEqual(plan["attached_budget"], 512)
            self.assertEqual(plan["live_capacity_limit"], 512)
            records = plan["records"]
            self.assertEqual([entry["status"] for entry in records],
                             ["admitted", "hidden", "hidden", "hidden", "unresolved", "admitted"])
            self.assertEqual([entry["owner"]["kind"] for entry in records],
                             ["idle_clip", "none", "none", "none", "none", "idle_clip"])
            for index in (0, 5):
                owner = records[index]["owner"]
                self.assertEqual(owner["clip_status"], "bound")
                self.assertEqual(owner["idle_animation"],
                                 "data/art/models/eawr_attached_host_idle_00.ala")
                self.assertEqual(owner["idle_animation_status"], "corpus_naming_observed")
                self.assertEqual(owner["idle_animation_provenance"]["tag"], "model_stem")
                self.assertEqual(owner["mesh_pose"], "bind")
                self.assertEqual(owner["bone"], 1)
                self.assertEqual(owner["samples"], 60)
                spawns, detaches, releases = self.owner_summary(owner)
                self.assertEqual(spawns, [0, 42])
                self.assertEqual(detaches, [(18, "draining")])
                self.assertEqual(len(releases), 1)
                self.assertTrue(18 < releases[0] < 60, releases)
                self.assertEqual((owner["generations"], owner["detaches"], owner["drains_released"],
                                  owner["drains_cut_short"]), (2, 1, 1, 0))
                self.assertLessEqual(owner["peak_live_instances"], 2)
                self.assertTrue(owner["released"])
                self.assertEqual(records[index]["runtime_status"], "drawn")
                self.assertGreater(records[index]["changed_pixels"], 0)
                self.assertEqual(owner, repeat["map_attached_effects"]["records"][index]["owner"])
                self.assertEqual(records[index]["stream_hash"],
                                 repeat["map_attached_effects"]["records"][index]["stream_hash"])
            particles = first["map_particles"]
            self.assertEqual(particles["advanced_frames"], 60)
            self.assertTrue(particles["evidence_verified"])
            self.assertEqual(particles["changed_pixels_outside_bounds"], 0)
            self.assertGreater(particles["live_resources_at_capture"], 0)
            self.assertEqual(particles["live_rids_after_release"], 0)
            self.assertEqual(particles["live_resources_after_release"], 0)
            self.assertEqual(first["evidence"]["capture_sha256"], repeat["evidence"]["capture_sha256"])

    def test_owner_hidden_at_first_sample_still_advances(self):
        # Clip frames 0-2 hidden: samples 0-8 have nothing live, so only the
        # has_work() gate keeps the clock running until the spawn at 9.
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-late-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True, idle_hidden_frames=(0, 1, 2))
            result = self.run_passing(root, directory / "late.json", *IDLE,
                                      "--eawr-map-effect-animation", "idle")
            self.assertEqual(result["map_particles"]["advanced_frames"], 60)
            for index in (0, 5):
                record = result["map_attached_effects"]["records"][index]
                spawns, detaches, releases = self.owner_summary(record["owner"])
                self.assertEqual((spawns, detaches, releases), ([9], [], []))
                self.assertEqual(record["owner"]["events"][0],
                                 {"sample": 0, "visible": False, "spawned_generation": None,
                                  "detached": None, "drains_released": 0, "drains_cut_short": 0,
                                  "live_instances": 0})
                self.assertEqual(record["runtime_status"], "drawn")
            self.assertEqual(result["map_particles"]["live_resources_after_release"], 0)

    def test_default_and_explicit_none_have_no_owner(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-none-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True)
            omitted = self.run_passing(root, directory / "omitted.json", *IDLE)
            explicit = self.run_passing(root, directory / "none.json", *IDLE,
                                        "--eawr-map-effect-animation", "none")
            for result in (omitted, explicit):
                plan = result["map_attached_effects"]
                self.assertNotIn("effect_animation", plan)
                self.assertTrue(all("owner" not in entry for entry in plan["records"]))
            self.assertEqual(omitted["map_attached_effects"], explicit["map_attached_effects"])
            self.assertEqual(omitted["evidence"]["capture_sha256"], explicit["evidence"]["capture_sha256"])
            self.assertEqual(omitted["map_particles"]["placements"], explicit["map_particles"]["placements"])

    def test_capacity_one_below_the_edge_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-capacity-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True)
            code, result = self.launch(root, directory / "short.json", *IDLE,
                                       "--eawr-map-particle-capacity", "511",
                                       "--eawr-map-effect-animation", "idle")
            self.assertEqual(code, 2)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(result["failure"].startswith("drain_headroom_exhausted"), result["failure"])
            plan = result["map_attached_effects"]
            self.assertEqual(plan["drain_headroom"], 0)
            self.assertTrue(all(entry["runtime_status"] == "not_spawned" for entry in plan["records"]))
            self.assertEqual(result["map_particles"]["placements"], [])

    def assert_accounted_empty(self, result, status):
        plan = result["map_attached_effects"]
        self.assertEqual(plan["owners_empty_at_capture"], 2)
        particles = result["map_particles"]
        self.assertTrue(particles["evidence_verified"])
        self.assertEqual(particles["changed_pixels_outside_bounds"], 0)
        # The comparison is captured even though nothing is drawn.
        self.assertTrue(particles["effects_off_capture_sha256"])
        self.assertEqual(particles["live_rids_after_release"], 0)
        self.assertEqual(particles["live_resources_after_release"], 0)
        for index in (0, 5):
            record = plan["records"][index]
            self.assertEqual(record["runtime_status"], status)
            self.assertEqual(record["changed_pixels"], 0)
            self.assertTrue(record["owner"]["released"])

    def test_late_reappearance_before_start_delay_is_accounted_empty(self):
        # A 0.5 s start delay: generation 0 emits from sample 15 and drains
        # after the hide at 18; generation 1 reappears at 42 and has no
        # particle yet when the clock stops after sample 55. The previous
        # build failed this with "has no drawn bounds".
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-delay-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True, effect_start_delay=0.5)
            result = self.run_passing(root, directory / "delay.json", *IDLE,
                                      "--eawr-map-particle-frames", "56",
                                      "--eawr-map-effect-animation", "idle")
            self.assert_accounted_empty(result, "live_no_particles")
            self.assertEqual(result["map_particles"]["advanced_frames"], 56)
            self.assertGreater(result["map_particles"]["live_resources_at_capture"], 0)
            for index in (0, 5):
                owner = result["map_attached_effects"]["records"][index]["owner"]
                spawns, detaches, releases = self.owner_summary(owner)
                self.assertEqual((spawns, detaches, releases), ([0, 42], [(18, "draining")], [32]))
                self.assertEqual(owner["events"][-1]["live_instances"], 1)

    def test_all_owners_hidden_in_the_final_phase(self):
        # Clip frames 12-30 hidden: both owners detach at 36 and their drains
        # end at 50, so nothing is live at capture. The previous build took
        # no effects-off capture and failed with an empty failure.
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-final-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True, idle_hidden_frames=tuple(range(12, 31)))
            result = self.run_passing(root, directory / "final.json", *IDLE,
                                      "--eawr-map-effect-animation", "idle")
            self.assert_accounted_empty(result, "hidden_by_clip")
            self.assertEqual(result["map_particles"]["live_resources_at_capture"], 0)
            for index in (0, 5):
                owner = result["map_attached_effects"]["records"][index]["owner"]
                spawns, detaches, releases = self.owner_summary(owner)
                self.assertEqual((spawns, detaches, releases), ([0], [(36, "draining")], [50]))

    def test_clip_that_never_shows_still_validates_its_emitters(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-never-") as temporary:
            directory = pathlib.Path(temporary)
            never = tuple(range(0, 31))
            good = fixture.write_fixture_root(directory / "good", idle=True, idle_hidden_frames=never)
            result = self.run_passing(good, directory / "never.json", *IDLE,
                                      "--eawr-map-effect-animation", "idle")
            self.assert_accounted_empty(result, "hidden_by_clip")
            for index in (0, 5):
                self.assertEqual(result["map_attached_effects"]["records"][index]["owner"]["generations"], 0)
            # The same clip with an unresolvable texture: no generation would
            # ever spawn, so only the prepare-time probe can refuse it.
            bad = fixture.write_fixture_root(directory / "bad", idle=True, idle_hidden_frames=never,
                                             effect_texture="p_eawr_missing_owner.tga")
            code, result = self.launch(bad, directory / "bad.json", *IDLE,
                                       "--eawr-map-effect-animation", "idle")
            self.assertEqual(code, 2)
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["failure"],
                             "one or more map particle placements failed; identities and causes are in map_particles")
            placements = result["map_particles"]["placements"]
            self.assertEqual(len(placements), 2)
            for placement in placements:
                self.assertEqual(placement["status"], "failed")
                self.assertEqual(placement["causes"], [
                    "owner prepare probe: emitter 0: backend could not create the emitter's resources: "
                    "colour texture 'p_eawr_missing_owner.tga' did not resolve"])
            self.assertEqual(result["map_particles"]["advanced_frames"], 0)

    def test_idle_refusals(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-refused-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "land", idle=True)
            idle = ("--eawr-map-effect-animation", "idle")
            cases = (
                ("effects-off", root, (*IDLE, "--eawr-map-effects", "off", *idle), True,
                 "--eawr-map-effect-animation idle requires --eawr-map-effects on"),
                ("fog", root, (*IDLE, "--eawr-fog-team", "2", *idle), True,
                 "--eawr-map-effect-animation idle is not supported with fog"),
                ("no-populate", root, (*IDLE, *idle), False,
                 "--eawr-map-effect-animation idle requires --eawr-populate"),
            )
            for name, game_root, args, populate, failure in cases:
                with self.subTest(name):
                    code, result = self.launch(game_root, directory / f"{name}.json", *args, populate=populate)
                    self.assertEqual(code, 2)
                    self.assertEqual(result["status"], "failed")
                    self.assertEqual(result["failure"], failure)
                    self.assertEqual(result["map_particles"]["placements"], [])
            import space_environment_fixture as space  # noqa: E402

            space_root = space.write_fixture_root(directory / "space")
            code, result = self.launch(space_root, directory / "space.json", *idle,
                                       map_path=space.MAP_LOGICAL_PATH)
            self.assertEqual(code, 2)
            self.assertEqual(result["failure"], "--eawr-map-effect-animation idle applies only to land maps")

    def test_option_errors(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-idle-options-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory, idle=True)
            cases = (
                (("--eawr-map-effect-animation", "walk"),
                 "--eawr-map-effect-animation expects none or idle"),
                (("--eawr-map-effect-animation",), "missing value for --eawr-map-effect-animation"),
            )
            for index, (args, failure) in enumerate(cases):
                code, result = self.launch(root, directory / f"option-{index}.json", *IDLE, *args)
                self.assertEqual(code, 2)
                self.assertEqual(result["failure"], failure)


if __name__ == "__main__":
    unittest.main()
