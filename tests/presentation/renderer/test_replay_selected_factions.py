"""Selected-faction skirmish recordings retain their strict replay content identity."""

import os
import json
import struct
import base64
import hashlib
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from battle_input_test_support import BattleInputRunner, CAMERA


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game")
class SelectedFactionReplay(BattleInputRunner, unittest.TestCase):
    def _round_trip(self, directory, name, slots=(), map_path="data/art/maps/_mp_space_coruscant.ted",
                    ticks=60, inputs=(), session="skirmish", camera=None, ai=False):
        replay = directory / f"{name}.eawr-replay"
        original_hashes = directory / f"{name}-original.csv"
        common = ["--eawr-live-ai", "on" if ai else "off", "--eawr-live-player", "1", "--eawr-live-step", "1",
                  "--eawr-audio", "off"]
        code, original = self._run(directory, name + "-record", common + list(slots) + list(inputs) + [
            "--eawr-live-reveal", "off", "--eawr-live-replay-out", str(replay),
            "--eawr-live-hashes", str(original_hashes)], camera=camera, end_tick=ticks,
            session=session, map_path=map_path)
        self.assertEqual(code, 0, original.get("failure"))
        # Rig receipts retain JSON/CSV, so preserve this small recording for independent CLI playback.
        data = replay.read_bytes()
        (directory / f"{name}-recording.json").write_text(json.dumps({
            "sha256": hashlib.sha256(data).hexdigest(), "content_identity": data[72:104].hex(),
            "original_hashes_csv": original_hashes.read_text(encoding="utf-8"),
            "bytes_base64": base64.b64encode(data).decode("ascii")}), encoding="utf-8")
        expected = original_hashes.read_bytes()
        comparisons = {}
        for reveal in ("off", "on"):
            actual = directory / f"{name}-{reveal}.csv"
            code, played = self._run(directory, name + "-" + reveal, common + [
                "--eawr-live-replay", str(replay), "--eawr-live-reveal", reveal,
                "--eawr-live-hashes", str(actual)], camera=camera, end_tick=ticks,
                session="replay", map_path="data/art/maps/_mp_space_coruscant.ted")
            self.assertEqual(code, 0, played.get("failure"))
            rows = actual.read_bytes()
            comparisons[reveal] = {"equal": rows == expected,
                "original_tick_1": expected.decode().splitlines()[1], "replayed_tick_1": rows.decode().splitlines()[1],
                "rejected": played["live_session"]["rejected"]}
        (directory / f"{name}-comparison.json").write_text(json.dumps(comparisons, indent=2), encoding="utf-8")
        self.assertTrue(all(item["equal"] for item in comparisons.values()), comparisons)
        self.assertTrue(all(item["rejected"] == original["live_session"]["rejected"]
                            for item in comparisons.values()), comparisons)
        return replay, original

    @staticmethod
    def _opcodes(replay):
        data = replay.read_bytes()
        header = struct.unpack_from("<H", data, 10)[0]
        players, squadrons, units = struct.unpack_from("<IIQ", data, 48)
        offset = header + players * 24 + units * 80
        for _ in range(squadrons):
            members = struct.unpack_from("<I", data, offset + 8)[0]
            offset += 16 + 8 * members
        opcodes = []
        while offset < len(data):
            opcodes.append(data[offset + 24])
            offset += 4 + struct.unpack_from("<I", data, offset)[0]
        return opcodes

    def test_ai_only_roles_reproduce_original_hashes(self):
        # Keep M2's map, factions, teams and fleets; change only the human role.
        # The reveal-off leg isolates role reconstruction from the reveal branch.
        with tempfile.TemporaryDirectory(prefix="eawr-replay-roles-") as temporary:
            self._round_trip(pathlib.Path(temporary), "ai-only", (
                "--eawr-skirmish-slot", "1:Rebel:0:ai"), ticks=2)

    def test_replay_rejects_malformed_input_before_loading_a_map(self):
        with tempfile.TemporaryDirectory(prefix="eawr-invalid-replay-") as temporary:
            directory = pathlib.Path(temporary)
            replay = directory / "truncated.eawr-replay"
            replay.write_bytes(b"EAWRPLY\0")
            code, result = self._run(directory, "refused", ["--eawr-live-replay", str(replay)],
                                     camera=None, end_tick=2, session="replay", map_path="")
            self.assertNotEqual(code, 0)
            self.assertIn("truncated replay", result["failure"])

    def test_recorded_lobbies_reproduce_every_hash_with_reveal_off_and_on(self):
        lobbies = {
            "human-human": ("1:Empire:0:human", "2:Rebel:1:human"),
            "human-ai": ("1:Empire:0:ai", "2:Rebel:1:human"),
            "two-v-two": ("1:Empire:0:ai", "2:Empire:0:human", "3:Rebel:1:human", "4:Rebel:1:ai"),
            "ffa": ("1:Empire:0:ai", "2:Rebel:1:human", "3:Underworld:2:ai"),
        }
        with tempfile.TemporaryDirectory(prefix="eawr-recorded-lobbies-") as temporary:
            directory = pathlib.Path(temporary)
            for name, slots in lobbies.items():
                with self.subTest(lobby=name):
                    args = ["--eawr-skirmish-players", ",".join(slot.split(":")[0] for slot in slots)]
                    for slot in slots: args += ["--eawr-skirmish-slot", slot]
                    args += ["--eawr-skirmish-seed", "67", "--eawr-skirmish-victory", "all-units"]
                    map_path = "data/art/maps/_mp_space_coruscant.ted" if name == "two-v-two" else "data/art/maps/_mp_space_ryloth.ted"
                    self._round_trip(directory, name, args, map_path=map_path)

    def test_recorded_reinforcement_reproduces_every_hash(self):
        with tempfile.TemporaryDirectory(prefix="eawr-recorded-reinforce-") as temporary:
            directory = pathlib.Path(temporary)
            # The authored Rebel station purchases an X-wing, then the HUD delivers it.
            replay, original = self._round_trip(directory, "reinforcement", ticks=720, session="m2", camera=CAMERA, inputs=(
                "--eawr-live-input", "10:click:unit=1", "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:press:hud=r_0000",
                "--eawr-live-input", "515:hover:@-4450,5050,0",
                "--eawr-live-input", "520:release:@-4450,5050,0"))
            self.assertEqual(original["battle_input"]["production"]["placements"], 1)
            opcodes = self._opcodes(replay)
            self.assertIn(9, opcodes)
            self.assertTrue(any(opcode in (11, 19) for opcode in opcodes))

    def test_ai_controllers_record_purchases_and_reinforcements_without_divergence(self):
        with tempfile.TemporaryDirectory(prefix="eawr-ai-replay-") as temporary:
            # Seed 67's first AI reinforcement is at tick 3520, after its station upgrades.
            replay, original = self._round_trip(pathlib.Path(temporary), "ai-controllers", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Empire:0:ai",
                "--eawr-skirmish-slot", "2:Rebel:1:ai", "--eawr-skirmish-seed", "67"), ticks=4200, ai=True)
            self.assertEqual(original["live_session"]["ai"]["players"], [1, 2])
            opcodes = self._opcodes(replay)
            # WAS-26 records AI purchases with prepaid opcode 24.
            self.assertTrue(any(opcode in (9, 24) for opcode in opcodes))
            self.assertTrue(any(opcode in (11, 19) for opcode in opcodes))

    def test_three_faction_recording_replays_and_rejects_a_changed_identity(self):
        with tempfile.TemporaryDirectory(prefix="eawr-faction-replay-") as temporary:
            directory = pathlib.Path(temporary)
            replay = directory / "recorded.eawr-replay"
            common = ["--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-step", "1",
                      "--eawr-live-reveal", "off", "--eawr-hud", "off", "--eawr-audio", "off"]
            map_path = "data/art/maps/_mp_space_ryloth.ted"
            code, recorded = self._run(directory, "record", common + [
                "--eawr-skirmish-players", "1,2,3",
                "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai",
                "--eawr-skirmish-slot", "3:Underworld:2:ai",
                "--eawr-live-replay-out", str(replay)],
                camera=None, end_tick=30, session="skirmish", map_path=map_path)
            self.assertEqual(code, 0, recorded.get("failure"))
            self.assertTrue(replay.is_file())
            code, played = self._run(directory, "play", common + ["--eawr-live-replay", str(replay)],
                                     camera=None, end_tick=30, session="replay", map_path=map_path)
            self.assertEqual(code, 0, played.get("failure"))
            self.assertTrue(played["live_session"]["headless_hashes_equal"])
            self.assertEqual(played["live_session"]["final_state_sha256"],
                             recorded["live_session"]["final_state_sha256"])
            self.assertEqual(played["live_session"]["rejected"], [])
            corrupted = bytearray(replay.read_bytes())
            corrupted[72] ^= 1
            changed = directory / "changed.eawr-replay"
            changed.write_bytes(corrupted)
            code, refused = self._run(directory, "refuse", common + ["--eawr-live-replay", str(changed)],
                                      camera=None, end_tick=30, session="replay", map_path=map_path)
            self.assertNotEqual(code, 0)
            self.assertIn("replay names other content", refused["failure"])


if __name__ == "__main__":
    unittest.main()
