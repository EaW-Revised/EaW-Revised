"""The parity doc's numbers come from melee_parity_analyze.py: pin its windows and phase sums."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "performance"))
import melee_parity_analyze as analyze  # noqa: E402

FIELDS = ["tick", "ticks", "tick_ms", "wall_frame_ms", "units", "projectiles"] + [
    k for k in ("frame_ms", "particle_ms", "render_cpu_ms", "render_gpu_ms", "viewer_main_ms",
                "outside_viewer_ms", "submit_ms", "bookkeeping_ms", "hud_ms", "audio_ms", "fog_ms",
                "pose_ms", "emitter_prepare_ms", "projectile_prepare_ms", "clone_prepare_ms",
                "tick_wait_ms", "live_frame_ms", "session_tail_ms", "emitter_frame_ms",
                "effects_frame_ms", "debris_frame_ms", "idle_frame_ms")]


class AnalyzeTests(unittest.TestCase):
    def test_viewer_whole_run_and_doc_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            with path.open("w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=FIELDS)
                writer.writeheader()
                for tick in range(1, 2400):
                    row = {k: 1 for k in FIELDS}
                    row.update(tick=tick, wall_frame_ms=10 + tick % 3)
                    writer.writerow(row)
            windows = {tuple(w["tick_window"]): w for w in analyze.viewer(path)["windows"]}
        for window in ((1, 300), (1, 2400), (1320, 1770), (1770, 2220), (1320, 2220)):
            self.assertIn(window, windows)
        self.assertEqual(windows[(1, 2400)]["samples"], 2399)
        self.assertEqual(windows[(1320, 2220)]["samples"], 900)

    def test_simulation_block_adds_its_phases(self):
        # Frames of boundaries 0, 7, 8, 11, 14, 15, 16, 17 at 1 ms spacing: only consecutive
        # pairs (7-8, 14-15, 15-16, 16-17) are phases; the gaps are merged spans and are skipped.
        boundaries = (0, 7, 8, 11, 14, 15, 16, 17)
        rows, now = [], 0
        for frame in range(4):
            for b in boundaries:
                rows.append((b, frame, 1, 1, now))
                now += 10_000
        result = analyze.phases(rows, 10_000_000)
        self.assertEqual(result["per_phase"]["advanceWait"]["frames"], 3)
        self.assertAlmostEqual(result["per_phase"]["advanceWait"]["mean_ms"], 1.0)
        self.assertAlmostEqual(result["simulation_block_ms"], 2.0)  # advanceWait + endFrameAndCrc
        self.assertIsNone(analyze.phases([(0, 0, 1, 0, t * 10_000) for t in range(5)], 10_000_000))

    def test_viewer_preserves_particle_cpu_and_commit_intervals(self):
        fields = FIELDS + ['effects_batch_step_cpu_ms', 'effects_commit_ms', 'particle_submission_ms']
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fine.csv'
            with path.open('w', newline='') as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                for tick, cpu, commit in ((1320, 2, 0.25), (1321, 4, 0.75)):
                    row = {k: 1 for k in fields}
                    row.update(tick=tick, effects_batch_step_cpu_ms=cpu, effects_commit_ms=commit,
                               particle_submission_ms=commit / 2)
                    writer.writerow(row)
            window = next(w for w in analyze.viewer(path)['windows'] if w['tick_window'] == [1320, 2220])
        self.assertEqual(window['timing_ms']['effects_batch_step_cpu_ms']['mean'], 3)
        self.assertEqual(window['timing_ms']['effects_commit_ms']['mean'], 0.5)
        self.assertEqual(window['timing_ms']['particle_submission_ms']['mean'], 0.25)


if __name__ == "__main__":
    unittest.main()
