"""Rig suite for the HUD facts / weapon-action HUD / command bar cuts (#4331): BASE and HEAD viewers on one GPU host.

Run through the normal rig queue with an exclusive GPU lease. The BASE viewer DLL arrives as an extra artifact:

    pwsh tools/rig/Invoke-RigSuite.ps1 -Suite tools/performance/gc_render_hud_facts_suite.py -Exclusive `
        -ExtraArtifact 'local/base.dll=gc/base.dll' `
        -Env 'EAWR_JOB_BASE_VIEWER_DLL={artifact:gc/base.dll}','EAWR_JOB_BASE_VIEWER_SHA256=<sha>'

The galaxy is the 250-planet / 12-faction synthetic galaxy of the scale probe (tools/performance/gc_scale_probe_gen.py,
generated from the host's installed game) and the stock Equal Footing campaign. Each case alternates the BASE and HEAD
viewer (the DLL is swapped in this job's private project; every engine process exits before the next DLL is installed).

  test_pixels_*   paused sessions at --fixed-fps 60: an overview capture and a keyboard pan out and back, captured at the
                  same frames. Taken twice per viewer (HEAD, BASE, HEAD, BASE): the PNGs must be byte-identical.
  test_evolving_* all-AI captures (one logical step per frame, forced seed): 3600 steps with captures at step 1800 and
                  3600, so the HUD facts, command bar and weapon HUD go through thousands of publications in which
                  owners, levels, production and income change. Twice per viewer; byte-identical across all four.
  test_cost       30 s unpaused runs, repeated and interleaved; per-phase ms/frame of both viewers.
"""
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "tools/performance").is_dir())
sys.path.insert(0, str(ROOT / "tools/performance"))
import gc_scale_probe_gen as gen  # noqa: E402
import gc_scale_probe_gpu as gpu  # noqa: E402

RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
BASE_DLL = os.environ.get("EAWR_JOB_BASE_VIEWER_DLL")
BASE_SHA = os.environ.get("EAWR_JOB_BASE_VIEWER_SHA256")
SECONDS = int(os.environ.get("EAWR_JOB_HUDFACTS_SECONDS", "30"))
REPEATS = int(os.environ.get("EAWR_JOB_HUDFACTS_REPEATS", "3"))
STEPS = int(os.environ.get("EAWR_JOB_HUDFACTS_STEPS", "3600"))
VERIFY_STEPS = int(os.environ.get("EAWR_JOB_HUDFACTS_VERIFY_STEPS", "1800"))
HEAD_ONLY = bool(os.environ.get("EAWR_JOB_HUDFACTS_HEAD_ONLY"))
INSTALLED = ROOT / "apps/viewer/project/bin/libeawr_viewer.windows.template_release.x86_64.dll"

STOCK = "Sandbox_Equal_Footing_Underworld"
SYNTH = "Synth_250_12"

PAN_DRIVER = '''extends SceneTree
var frame = 0
var plan = []
var codes = {"RIGHT": KEY_RIGHT, "LEFT": KEY_LEFT, "UP": KEY_UP, "DOWN": KEY_DOWN, "HOME": KEY_HOME}
func _initialize():
    plan = JSON.parse_string(FileAccess.get_file_as_string(__PLAN__))
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _key(name, pressed):
    var event = InputEventKey.new()
    event.keycode = codes[name]
    event.physical_keycode = codes[name]
    event.pressed = pressed
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    for step in plan:
        if int(step[0]) != frame:
            continue
        if step[1] == "key":
            _key(step[2], step[3])
        elif step[1] == "capture":
            root.get_texture().get_image().save_png(step[2])
        elif step[1] == "quit":
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
    return false
'''


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pan_plan(directory, hold):
    """Frames at 60 fps: capture the overview, pan right, up, back left and down, capturing after every leg."""
    steps, frame, names = [], 300, []

    def capture(name):
        names.append(name)
        steps.append([frame, "capture", str(directory / (name + ".png")).replace("\\", "/")])

    capture("overview")
    for leg, key in (("right", "RIGHT"), ("up", "UP"), ("left", "LEFT"), ("left2", "LEFT"), ("down", "DOWN"), ("down2", "DOWN")):
        frame += 3
        steps.append([frame, "key", key, True])
        frame += hold
        steps.append([frame, "key", key, False])
        frame += 12  # let the release land and a frame draw
        capture(leg)
    steps.append([frame + 3, "quit"])
    return steps, names


@unittest.skipUnless(RUNTIME and GAME_ROOT and (HEAD_ONLY or (BASE_DLL and BASE_SHA)), "requires exclusive GPU, FoC and the BASE viewer DLL")
class HudFactsPair(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = Path(tempfile.mkdtemp(prefix="eawr-hud-facts-"))
        cls.head = cls.directory / "head.dll"
        shutil.copyfile(INSTALLED, cls.head)
        cls.head_sha = sha256(cls.head)
        cls.libraries = {"head": cls.head}
        if not HEAD_ONLY:
            assert sha256(BASE_DLL) == BASE_SHA.lower(), "the BASE viewer DLL is not the one named"
            cls.libraries["base"] = Path(BASE_DLL)
            assert cls.head_sha != BASE_SHA.lower(), "HEAD and BASE are the same DLL"
        xml = gen.extract_effective_xml(Path(GAME_ROOT), cls.directory / "stock-xml")
        cls.mod = cls.directory / "mod-250-12"
        gen.generate(gen.Stock(xml), cls.mod, 250, 12, 4286, "even")
        # All-AI captures need an AI controller for the local faction too: the same galaxy with Underworld AI-controlled.
        cls.mod_all_ai = cls.directory / "mod-250-12-all-ai"
        shutil.copytree(cls.mod, cls.mod_all_ai)
        campaigns = cls.mod_all_ai / "Data/XML/campaigns_underworld_gc.xml"
        body = campaigns.read_text(encoding="utf-8")
        anchor = "<AI_Player_Control> Pirates, None </AI_Player_Control>"
        assert body.count(anchor) >= 1
        campaigns.write_text(body.replace(anchor, anchor + "\n\t\t<AI_Player_Control> Underworld, AI_Player_Underworld </AI_Player_Control>"), encoding="utf-8")
        sys.path.insert(0, str(ROOT))
        from tools.capture.gc_ai_proof import make_overlay  # noqa: E402
        cls.stock_all_ai = make_overlay(GAME_ROOT, STOCK, cls.directory / "overlay-stock-all-ai", "AI_Player_Underworld")

    @classmethod
    def tearDownClass(cls):
        shutil.copyfile(cls.head, INSTALLED)
        assert sha256(INSTALLED) == cls.head_sha, "the HEAD viewer was not restored"

    def install(self, label):
        shutil.copyfile(self.libraries[label], INSTALLED)
        self.assertEqual(sha256(INSTALLED), self.head_sha if label == "head" else BASE_SHA.lower())

    def engine(self, label, out, campaign, mod, reveal, ai, script, extra_engine=()):
        self.install(label)
        report = out / "report.json"
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", *extra_engine,
                "--path", str(ROOT / "apps/viewer/project")]
        if script:
            args += ["--script", str(script)]
        args += ["--", "--eawr-galactic", campaign, "--eawr-game-root", GAME_ROOT]
        if mod:
            args += ["--eawr-mod-root", str(mod)]
        args += ["--eawr-galactic-seat", "1", "--eawr-audio", "off", "--eawr-galactic-reveal", reveal,
                 "--eawr-report", str(report)]
        if ai == "off":
            args += ["--eawr-galactic-ai", "off"]
        return args, report

    def compare(self, name, images, results):
        """results: {label: {"out": dir, ...}} where label is head, base, head2, base2; every image must match base."""
        from PIL import Image, ImageChops
        rows = []
        for image in images:
            files = {label: result["out"] / (image + ".png") for label, result in results.items()}
            for label, path in files.items():
                self.assertTrue(path.is_file(), f"{name}/{image}: {label} wrote no capture")
            reference = files["base"]
            for label, path in files.items():
                if label == "base":
                    continue
                same_bytes = path.read_bytes() == reference.read_bytes()
                bounds = None
                with Image.open(path) as a, Image.open(reference) as b:
                    self.assertEqual(a.size, b.size)
                    difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
                    bounds = difference.getbbox()
                    if bounds:
                        difference.save(self.directory / f"{name}-{image}-{label}-difference.png")
                rows.append({"image": image, "label": label, "byte_equal": same_bytes, "pixel_difference_bounds": bounds})
        return rows

    def finish_pairs(self, name, campaign, reveal, rows, results):
        record = {"case": name, "campaign": campaign, "reveal": reveal, "images": rows,
                  "head_work": results["head"]["work"], "base_work": results["base"]["work"],
                  "head_sha256": self.head_sha, "base_sha256": (BASE_SHA or "").lower()}
        (self.directory / f"pixels-{name}.json").write_text(json.dumps(record, indent=1), encoding="utf-8")
        print("HUD_FACTS_PIXELS", json.dumps({"case": name, "images": len(rows),
              "all_byte_equal": all(r["byte_equal"] for r in rows), "head_work": results["head"]["work"]}), flush=True)
        for row in rows:
            self.assertIsNone(row["pixel_difference_bounds"],
                f"{name}/{row['image']}/{row['label']}: changed pixels, see {self.directory}")
            self.assertTrue(row["byte_equal"], f"{name}/{row['image']}/{row['label']}: PNG bytes differ, pixels equal")
        return record

    def pan_case(self, name, campaign, mod, reveal, hold=40):
        results = {}
        for label, library in (("head", "head"), ("base", "base"), ("head2", "head"), ("base2", "base")):
            out = self.directory / f"{name}-{label}"
            out.mkdir()
            plan, names = pan_plan(out, hold)
            plan_path = out / "plan.json"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            driver = out / "pan.gd"
            driver.write_text(PAN_DRIVER.replace("__PLAN__", json.dumps(str(plan_path).replace("\\", "/"))), encoding="utf-8")
            args, report = self.engine(library, out, campaign, mod, reveal, "off", driver, ("--fixed-fps", "60"))
            args += ["--eawr-galactic-paused", "yes"]
            run = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=1200, check=False)
            (out / "viewer.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            self.assertEqual(run.returncode, 0, run.stdout[-1500:] + run.stderr[-1500:])
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertFalse(data["failure"], data["failure"])
            results[label] = {"out": out, "names": names, "work": data["hud_work"], "report": data}
        rows = self.compare(name, results["head"]["names"], results)
        return self.finish_pairs(name, campaign, reveal, rows, results)

    def evolving_case(self, name, campaign, mod, reveal):
        """All-AI capture: one logical step per frame, so every publication goes through the HUD."""
        results = {}
        for label, library in (("head", "head"), ("base", "base"), ("head2", "head"), ("base2", "base")):
            out = self.directory / f"{name}-{label}"
            out.mkdir()
            args, report = self.engine(library, out, campaign, mod, reveal, "on", None, ("--fixed-fps", "60"))
            args += ["--eawr-galactic-all-ai", "yes", "--eawr-galactic-seed", "37", "--eawr-galactic-ticks", str(STEPS),
                     "--eawr-capture", str(out / "shot.png")]
            run = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=3000, check=False)
            (out / "viewer.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            self.assertEqual(run.returncode, 0, run.stdout[-1500:] + run.stderr[-1500:])
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertFalse(data["failure"], data["failure"])
            frames = sorted(p.name[:-4] for p in out.glob("shot-frame-*.png"))
            self.assertGreaterEqual(len(frames), 2, f"{name}/{label}: expected captures at steps 1800 and 3600, got {frames}")
            results[label] = {"out": out, "names": frames, "work": data["hud_work"], "report": data}
        names = results["head"]["names"]
        for label, result in results.items():
            self.assertEqual(result["names"], names, f"{name}/{label}: capture frames differ")
        # Captures of a running galaxy differ from run to run (planets turn on the wall clock: BASE against BASE differs
        # too), so the HUD is compared as data: every report section but the wall-clock ones must be equal across all four.
        noise = ("animation_time", "animation_updates", "capture", "frames", "hud_work", "orbit_hud", "route_geometry",
                 "presentation_phases", "scene_layers", "weapon_action_hud", "perf", "live_session")
        stable = {label: {k: v for k, v in result["report"].items() if k not in noise} for label, result in results.items()}
        for label in ("head", "head2", "base2"):
            self.assertEqual(stable[label], stable["base"], f"{name}/{label}: the HUD report differs from BASE")
        self.assertGreater(len(stable["base"].get("intelligence", stable["base"])), 0)
        record = {"case": name, "sections": sorted(stable["base"]), "head_work": results["head"]["work"],
                  "base_work": results["base"]["work"]}
        (self.directory / f"evolving-{name}.json").write_text(json.dumps(record, indent=1), encoding="utf-8")
        print("HUD_FACTS_EVOLVING", json.dumps({"case": name, "sections": len(stable["base"]), "equal": True,
              "head_work": results["head"]["work"]}), flush=True)
        # The hash of the final simulation state must not move (presentation only).
        hashes = {label: (result["report"].get("ai_proof") or {}).get("end_state_hash") for label, result in results.items()}
        print("HUD_FACTS_STATE_HASH", json.dumps(hashes), flush=True)
        self.assertEqual(len({h for h in hashes.values()}), 1, f"{name}: end state hashes differ {hashes}")
        return record

    def test_pixels_stock_fog_on(self):
        self.pan_case("stock-fog-on", STOCK, None, "off")

    def test_pixels_stock_fog_off(self):
        self.pan_case("stock-fog-off", STOCK, None, "on")

    def test_pixels_synthetic_fog_on(self):
        self.pan_case("synth-fog-on", SYNTH, self.mod, "off")

    def test_pixels_synthetic_fog_off(self):
        self.pan_case("synth-fog-off", SYNTH, self.mod, "on")

    def test_evolving_stock_fog_on(self):
        self.evolving_case("evolve-stock-fog-on", STOCK, self.stock_all_ai, "off")

    def test_evolving_stock_fog_off(self):
        self.evolving_case("evolve-stock-fog-off", STOCK, self.stock_all_ai, "on")

    def test_evolving_synthetic_fog_on(self):
        self.evolving_case("evolve-synth-fog-on", SYNTH, self.mod_all_ai, "off")

    def test_evolving_synthetic_fog_off(self):
        self.evolving_case("evolve-synth-fog-off", SYNTH, self.mod_all_ai, "on")

    def verify_case(self, name, campaign, mod, reveal):
        """HEAD only: every publication is checked against a cold rebuild (--eawr-galactic-hud-verify)."""
        out = self.directory / name
        out.mkdir()
        args, report = self.engine("head", out, campaign, mod, reveal, "on", None, ("--fixed-fps", "60"))
        args += ["--eawr-galactic-all-ai", "yes", "--eawr-galactic-seed", "37", "--eawr-galactic-ticks", str(VERIFY_STEPS),
                 "--eawr-galactic-hud-verify", "yes", "--eawr-capture", str(out / "shot.png")]
        run = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=3000, check=False)
        (out / "viewer.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        self.assertEqual(run.returncode, 0, run.stdout[-1500:] + run.stderr[-1500:])
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["failure"], data["failure"])
        work = data["hud_work"]
        print("HUD_FACTS_VERIFY", json.dumps({"case": name, "work": work, "weapon": data.get("weapon_action_hud", {}).get("writes")}), flush=True)
        self.assertGreater(work["verify_checks"], VERIFY_STEPS, "every publication and every skipped update is checked")
        self.assertEqual(work["verify_mismatches"], 0, f"{name}: retained HUD state differs from a rebuild, see {out}")
        self.assertGreater(work["fact_changes"], 0, f"{name}: no fact changed during the run, the check proved nothing")
        return work

    def test_verify_stock_fog_on(self):
        self.verify_case("verify-stock-fog-on", STOCK, self.stock_all_ai, "off")

    def test_verify_stock_fog_off(self):
        self.verify_case("verify-stock-fog-off", STOCK, self.stock_all_ai, "on")

    def test_verify_synthetic_fog_on(self):
        self.verify_case("verify-synth-fog-on", SYNTH, self.mod_all_ai, "off")

    def test_verify_synthetic_fog_off(self):
        self.verify_case("verify-synth-fog-off", SYNTH, self.mod_all_ai, "on")

    def measure(self, label, campaign, mod, reveal, ai, repeat):
        out = self.directory / f"cost-{campaign}-reveal-{reveal}-ai-{ai}-{label}-{repeat}"
        out.mkdir()
        samples_path = out / "samples.json"
        driver = out / "measure.gd"
        driver.write_text(gpu.DRIVER.replace("__WARMUP__", "3000000").replace("__MEASURE__", str(SECONDS * 1_000_000)).replace(
            "__SAMPLES__", json.dumps(str(samples_path).replace("\\", "/"))).replace(
            "__SHOT__", json.dumps(str(out / "final.png").replace("\\", "/"))), encoding="utf-8")
        args, report = self.engine(label, out, campaign, mod, reveal, ai, driver, ("--disable-vsync",))
        args += ["--eawr-galactic-paused", "no", "--eawr-perf-overlay", "on"]
        run = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=1800, check=False)
        (out / "viewer.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        self.assertEqual(run.returncode, 0, run.stdout[-1500:] + run.stderr[-1500:])
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["failure"], data["failure"])
        samples = json.loads(samples_path.read_text(encoding="utf-8"))
        self.assertGreater(len(samples), 30)
        phases = {row["name"]: row for row in data["presentation_phases"]}
        frame_ms = [s["frame_ms"] for s in samples]
        return {"label": label, "campaign": campaign, "reveal": reveal, "ai": ai, "repeat": repeat,
                "frames": len(samples), "frame_ms": gpu.summarize(frame_ms),
                "process_ms": gpu.summarize([s["process_ms"] for s in samples]),
                "phases": {name: {"avg_ms": row["avg_ms"], "calls": row["calls"], "total_ms": row["total_ms"]}
                           for name, row in phases.items()},
                "hud_work": data["hud_work"], "planets": len(data["planets"]), "fleets": len(data["fleets"]),
                "session": {k: data.get("live_session", {}).get(k) for k in ("session_tick", "galactic_frame", "snapshots_presented")}}

    def test_cost(self):
        picked = os.environ.get("EAWR_JOB_HUDFACTS_CASE_IDS")
        cases = ((SYNTH, self.mod, "on", "off"), (SYNTH, self.mod, "off", "off"), (SYNTH, self.mod, "on", "on"),
                 (SYNTH, self.mod, "off", "on"), (STOCK, None, "off", "on"), (STOCK, None, "on", "on"))
        if picked:
            cases = tuple(cases[int(i)] for i in picked.split(","))
        rows = []
        labels = ("head",) if HEAD_ONLY else ("base", "head")
        extra = [n for n in os.environ.get("EAWR_JOB_HUDFACTS_PHASES", "").split(",") if n]
        for campaign, mod, reveal, ai in cases:
            for repeat in range(REPEATS):
                order = labels if repeat % 2 == 0 else tuple(reversed(labels))
                for label in order:
                    row = self.measure(label, campaign, mod, reveal, ai, repeat)
                    rows.append(row)
                    shown = {name: [round(row["phases"][name]["avg_ms"], 4), row["phases"][name]["calls"],
                                    round(row["phases"][name]["total_ms"], 3)]
                             for name in extra if name in row["phases"]}
                    print("HUD_FACTS_COST", json.dumps({
                        "label": label, "campaign": campaign, "reveal": reveal, "ai": ai, "repeat": repeat,
                        "frame_avg_ms": row["frame_ms"]["avg"], "frame_p50_ms": row["frame_ms"]["p50"],
                        "facts_ms": row["phases"]["hud_facts"]["avg_ms"], "commands_ms": row["phases"]["hud_commands"]["avg_ms"],
                        "extra": shown, "work": {k: row["hud_work"].get(k) for k in (
                            "fact_rebuilds", "command_updates", "offer_rows_visited", "max_offer_rows")},
                        "tick": row["session"]["session_tick"]}), flush=True)
                    (self.directory / "cost-matrix.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
        summary = []
        for campaign, mod, reveal, ai in cases:
            line = {"campaign": campaign, "reveal": reveal, "ai": ai}
            for label in labels:
                chosen = [r for r in rows if (r["campaign"], r["reveal"], r["ai"], r["label"]) == (campaign, reveal, ai, label)]
                names = sorted({n for r in chosen for n in r["phases"] if r["phases"][n]["calls"]})
                line[label] = {"frame_p50_ms": statistics.median(r["frame_ms"]["p50"] for r in chosen),
                               "frame_avg_ms": statistics.median(r["frame_ms"]["avg"] for r in chosen),
                               "phases": {phase: statistics.median(r["phases"][phase]["avg_ms"] for r in chosen
                                                                   if phase in r["phases"]) for phase in names}}
            summary.append(line)
        (self.directory / "cost-summary.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
        print("HUD_FACTS_SUMMARY", json.dumps(summary), flush=True)


if __name__ == "__main__":
    unittest.main()
