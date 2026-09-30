"""Independent verifier and opt-in graphical runs for the real MC-50 Hull shadow probe.

The probe (tests/presentation/lighting/renderer/hull_asset_shadow_probe.cpp)
renders the pinned Remake MC-50 Hull through production GodotRenderer and
writes its CPU ray-traced masks (masks.pgm: 0 excluded, 1 lit control,
2 caster-shadowed receiver), six PNGs and a JSON report into a private output
directory. This verifier reopens every file: it checks PNG CRCs and filters,
recomputes both mask means from the decoded pixels, re-hashes the mask, and
derives the verdicts itself.

Graphical runs: EAWR_GODOT_HULL_ASSET_SHADOW_TEST=1,
EAWR_GODOT_EXECUTABLE=<pinned Godot console>,
EAWR_HULL_ASSET_SHADOW_PROBE_LIBRARY=<built extension>,
EAWR_EAW_GAME_ROOT=<installation>, EAWR_REMAKE_MOD_ROOT=<Remake workshop item>.
EAWR_HULL_ASSET_SHADOW_OUTPUT optionally retains reports, masks, PNGs, the
verdict and review images (amplified on/off drop, mask overlay); they are
private: keep them out of Git. EAWR_HULL_SHADOW_MASKS_TESTS=<root
hull_shadow_masks_tests> optionally regenerates the mask on the CPU and
byte-compares it with the probe's. Every run is verified against the declared
fixture (HULL_FIXTURE) and its run label, so a diagnostic flag fails it.

The skinning control (docs/rendering.md#hull-shadow-fixture)
repeats both runs with `--eawr-hull-upload unskinned`: the same Hull surface,
masks, camera, light and casting setting, uploaded without its bone binding.
compare_routes() measures the matched captures pixel by pixel.
"""

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hull_shadow_fixtures import (compare_routes, CONFIGURATION_MUTATIONS, HARNESS, pinned, PROJECT,
    read_png, sample, verify, WIDTH, write_png, write_review_images)  # noqa: E402


class VerifierTests(unittest.TestCase):
    def test_positive_negative_and_signature(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as temporary:
            directory = Path(temporary)
            report = sample(directory)
            errors, verdict = verify(report, directory, "noncasting_receiver", pinned(directory))
            self.assertEqual(errors, [])
            self.assertTrue(verdict["means_controls_pass"] and verdict["geometric_agreement_pass"])
            self.assertFalse(verdict["self_shadow_signature"])
            # Only the real mask's hash separates the sample from the declared fixture.
            self.assertEqual(verify(report, directory, "noncasting_receiver")[0], ["mask SHA-256 pin"])
            mutations = [
                lambda r: r["captures"][0]["receiver"]["rgb"].__setitem__(0, 90),
                lambda r: r["captures"][1].__setitem__("name", "on"),
                lambda r: r["asset"].__setitem__("model_sha256", "0" * 64),
                lambda r: r["receiving_materials"].__setitem__("variant_failures", 1),
                lambda r: r["masks"].__setitem__("sha256", "0" * 64),
                lambda r: r["view_selection"].__setitem__("selected", 3),
                lambda r: r["shadows"].__setitem__("max_distance", 400),
                lambda r: r.__setitem__("status", "failed"),
                lambda r: r["failures"].append("fixture: missing asset"),
                lambda r: r["asset"].__setitem__("triangles", 13000),
                lambda r: r["upload"].__setitem__("skin_pose_probe", "rejected"),
                lambda r: r["upload"].__setitem__("mesh_bone", -1),
                lambda r: r["upload"].__setitem__("route", "palette"),
            ]
            for index, mutate in enumerate(mutations):
                changed = copy.deepcopy(report)
                mutate(changed)
                self.assertTrue(verify(changed, directory, "noncasting_receiver", pinned(directory))[0],
                                f"mutation {index}")
            for label in ("self_casting_receiver", "diagnostic"):
                self.assertIn("receiver casting flag contradicts run label",
                              verify(report, directory, label, pinned(directory))[0])
            (directory / "masks.pgm").write_bytes(b"P5\n1 1\n2\n\x00")
            self.assertTrue(any(error.startswith("mask")
                                for error in verify(report, directory, "noncasting_receiver")[0]))
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as temporary:
            directory = Path(temporary)
            report = sample(directory, self_shadow=True)
            errors, verdict = verify(report, directory, "self_casting_receiver", pinned(directory))
            self.assertEqual(errors, [])
            self.assertFalse(verdict["means_controls_pass"])
            self.assertTrue(verdict["self_shadow_signature"])
            self.assertIn("receiver casting flag contradicts run label",
                          verify(report, directory, "noncasting_receiver", pinned(directory))[0])
            (directory / "on.png").write_bytes(b"not PNG")
            self.assertTrue(any("on PNG" in error
                                for error in verify(report, directory, "self_casting_receiver", pinned(directory))[0]))

    def test_rejects_diagnostic_configurations(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as temporary:
            directory = Path(temporary)
            report = sample(directory)
            for name, (mutate, expected) in CONFIGURATION_MUTATIONS.items():
                changed = copy.deepcopy(report)
                mutate(changed)
                errors, verdict = verify(changed, directory, "noncasting_receiver", pinned(directory))
                self.assertIn(expected, errors, name)
                # The pixels still pass both gates: without the configuration
                # checks this run would read as acceptance evidence.
                self.assertTrue(verdict["means_controls_pass"] and verdict["geometric_agreement_pass"], name)

    def test_changed_mask_fails_the_pin(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as first, \
                tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as second:
            sample(Path(first))
            declared = pinned(Path(first))
            moved = sample(Path(second), shift=40)
            # Self-consistent evidence (file, hash, counts and pixels agree) for another mask.
            self.assertEqual(verify(moved, Path(second), "noncasting_receiver", pinned(Path(second)))[0], [])
            self.assertEqual(verify(moved, Path(second), "noncasting_receiver", declared)[0], ["mask SHA-256 pin"])

    def test_review_images(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-verifier-") as temporary:
            directory = Path(temporary)
            sample(directory)
            write_review_images(directory)
            drop = read_png(directory / "on_off_drop_x12.png")
            overlay = read_png(directory / "mask_overlay.png")
            receiver = (200 * WIDTH + 300) * 4
            control = (200 * WIDTH + 100) * 4
            self.assertEqual(bytes(drop[receiver:receiver + 3]), bytes((255, 255, 255)))
            self.assertEqual(bytes(drop[control:control + 3]), bytes((0, 0, 0)))
            self.assertEqual(bytes(overlay[receiver:receiver + 3]), bytes((0, 200, 0)))
            self.assertEqual(bytes(overlay[control:control + 3]), bytes((40, 90, 255)))

    def test_route_comparison(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-routes-") as temporary:
            rigid_dir, plain_dir = Path(temporary) / "rigid", Path(temporary) / "plain"
            rigid_dir.mkdir()
            plain_dir.mkdir()
            rigid = sample(rigid_dir, self_shadow=True)
            plain = sample(plain_dir, self_shadow=True, route="unskinned")
            self.assertEqual(verify(plain, plain_dir, "self_casting_receiver_unskinned", pinned(plain_dir))[0], [])
            # The route belongs to the label: an unskinned run is not a rigid one.
            self.assertEqual(verify(plain, plain_dir, "self_casting_receiver", pinned(plain_dir))[0],
                             ["upload route contradicts run label"])
            matched = compare_routes((rigid, rigid_dir), (plain, plain_dir))
            self.assertTrue(matched["matched"])
            self.assertEqual({c["differing_pixels"] for c in matched["captures"].values()}, {0})
            # A one-level control-pixel rounding difference is allowed.
            pixels = read_png(plain_dir / "on.png")
            control = 200 * WIDTH + 100
            pixels[control * 4] -= 1
            write_png(plain_dir / "on.png", pixels)
            self.assertTrue(compare_routes((rigid, rigid_dir), (plain, plain_dir))["matched"])
            # One changed receiver pixel breaks the matched-route gate.
            pixels = read_png(rigid_dir / "on.png")
            index = 220 * WIDTH + 350
            pixels[index * 4:index * 4 + 3] = bytes((140, 140, 140))
            write_png(plain_dir / "on.png", pixels)
            changed = compare_routes((rigid, rigid_dir), (plain, plain_dir))
            self.assertFalse(changed["matched"])
            self.assertEqual(changed["captures"]["on"]["differing_in_masks"], 1)
            # Comparing a route with itself is not a control.
            self.assertFalse(compare_routes((rigid, rigid_dir), (rigid, rigid_dir))["matched"])


RUNTIME_ENV = ("EAWR_GODOT_HULL_ASSET_SHADOW_TEST", "EAWR_GODOT_EXECUTABLE",
               "EAWR_HULL_ASSET_SHADOW_PROBE_LIBRARY", "EAWR_EAW_GAME_ROOT", "EAWR_REMAKE_MOD_ROOT")

# The documented outcome of this slice (docs/rendering.md#hull-shadow-fixture).
# #57 reversed the ALO indices at Godot upload so the Hull exterior draws;
# the pre-#57 geometric disagreement and self-shadow signature are gone.
EXPECTED = {
    "noncasting_receiver": {"means_controls_pass": True, "geometric_agreement_pass": True,
                            "self_shadow_signature": False},
    "self_casting_receiver": {"means_controls_pass": True, "geometric_agreement_pass": True,
                              "self_shadow_signature": False},
}
# The skinning control (docs/rendering.md#hull-shadow-fixture):
# the unskinned upload reproduces both verdicts, so skinning is not causal.
EXPECTED["noncasting_receiver_unskinned"] = EXPECTED["noncasting_receiver"]
EXPECTED["self_casting_receiver_unskinned"] = EXPECTED["self_casting_receiver"]
RUN_ARGUMENTS = {
    "noncasting_receiver": ["--eawr-receiver-casts", "false"],
    "self_casting_receiver": [],
    "noncasting_receiver_unskinned": ["--eawr-receiver-casts", "false", "--eawr-hull-upload", "unskinned"],
    "self_casting_receiver_unskinned": ["--eawr-hull-upload", "unskinned"],
}


@unittest.skipUnless(all(os.environ.get(name) for name in RUNTIME_ENV),
                     "set " + ", ".join(RUNTIME_ENV) + " for the graphical real-Hull runs")
class GraphicalProbe(unittest.TestCase):
    def run_probe(self, label):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-runtime-") as temporary:
            return self.run_in(label, Path(temporary))[1]

    def run_in(self, label, directory):
        """Run one labelled probe inside `directory`; return (report, verdict, output directory)."""
        executable = os.environ["EAWR_GODOT_EXECUTABLE"]
        library = os.environ["EAWR_HULL_ASSET_SHADOW_PROBE_LIBRARY"]
        self.assertTrue(Path(executable).is_file(), "pinned Godot console missing")
        self.assertTrue(Path(library).is_file(), "built hull asset extension missing")
        project = directory / "project"
        shutil.copytree(PROJECT, project)
        (project / "bin").mkdir()
        shutil.copy2(library, project / "bin" / Path(library).name)
        (project / ".godot").mkdir()
        (project / ".godot" / "extension_list.cfg").write_text(
            "res://eawr_hull_asset_shadow_probe.gdextension\n", encoding="utf-8")
        output = directory / "out"
        output.mkdir()
        report_path = output / "hull-asset-shadow.json"
        result = subprocess.run(
            [executable, "--path", str(project), "--rendering-driver", "vulkan", "--",
             "--eawr-lighting-report", str(report_path), "--eawr-lighting-output", str(output),
             "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
             "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"], *RUN_ARGUMENTS[label]],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300, check=False)
        log = result.stdout + "\n" + result.stderr
        destination = os.environ.get("EAWR_HULL_ASSET_SHADOW_OUTPUT")
        if destination:
            target = Path(destination) / label
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(output, target)
            (target / "godot.log").write_text(log, encoding="utf-8")
        self.assertIsNotNone(HARNESS.engine_banner(result.stdout, HARNESS.PINNED_GODOT_IDENTITY), log)
        self.assertTrue(report_path.is_file(), log)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        errors, verdict = verify(report, output, label)
        self.assertEqual(errors, [], json.dumps(report, indent=2))
        self.assertEqual(result.returncode, 0 if verdict["means_controls_pass"] else 1, log)
        if destination:
            (Path(destination) / label / "verdict.json").write_text(
                json.dumps(verdict, indent=2), encoding="utf-8")
            write_review_images(Path(destination) / label)
        masks_tool = os.environ.get("EAWR_HULL_SHADOW_MASKS_TESTS")
        if masks_tool:
            # Regenerate the mask with the root CPU build and byte-compare.
            cpu = directory / "cpu"
            regenerated = subprocess.run(
                [masks_tool, "--game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                 "--mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"], "--mask-output", str(cpu)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600, check=False)
            self.assertEqual(regenerated.returncode, 0, regenerated.stdout)
            self.assertEqual((cpu / "masks.pgm").read_bytes(), (output / "masks.pgm").read_bytes(),
                             "root CPU build and probe extension masks differ")
        observed = {key: verdict[key] for key in EXPECTED[label]}
        self.assertEqual(observed, EXPECTED[label], json.dumps(verdict, indent=2))
        return report, verdict, output

    def test_noncasting_receiver(self):
        self.run_probe("noncasting_receiver")

    def test_self_casting_receiver(self):
        self.run_probe("self_casting_receiver")

    def test_skinning_route_control(self):
        """Same Hull, scene, light, casting setting and GPU; only the upload's bone binding differs."""
        for casting in ("noncasting_receiver", "self_casting_receiver"):
            with tempfile.TemporaryDirectory(prefix="eawr-hull-asset-routes-") as temporary:
                runs = {}
                for label in (casting, casting + "_unskinned"):
                    work = Path(temporary) / label
                    work.mkdir()
                    runs[label] = self.run_in(label, work)
                rigid, plain = runs[casting], runs[casting + "_unskinned"]
                comparison = compare_routes((rigid[0], rigid[2]), (plain[0], plain[2]))
                comparison["verdicts"] = [rigid[1], plain[1]]
                destination = os.environ.get("EAWR_HULL_ASSET_SHADOW_OUTPUT")
                if destination:
                    (Path(destination) / (casting + "_route_comparison.json")).write_text(
                        json.dumps(comparison, indent=2), encoding="utf-8")
                self.assertTrue(comparison["matched"], json.dumps(comparison, indent=2))
                # run_in checked each route's declared verdict; the pixel gate
                # above bounds their raw measurements without float equality.


if __name__ == "__main__":
    unittest.main()
