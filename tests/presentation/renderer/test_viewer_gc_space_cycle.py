"""Genuine campaign admission, borrowed planet map and released child presentation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from .campaign_space_cases import qualified_campaign_arguments
    from .prepared_campaign_view_cases import PreparedCampaignViewContracts
except ImportError:
    import test_viewer_planet_hud_runtime as support
    from campaign_space_cases import qualified_campaign_arguments
    from prepared_campaign_view_cases import PreparedCampaignViewContracts
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.inventory.corpus import Corpus


def faction_colours():
    """Factions.xml `Color` and `No_Colorization_Color` RGB per lower-case faction name."""
    corpus, colours = Corpus(support.GAME_ROOT), {}
    for filename in ("factions.xml", "expansion_factions.xml"):
        source = corpus.read_effective("foc", "data/xml/" + filename)
        if source is None:
            continue
        for faction in ET.fromstring(source.data):
            row = colours.setdefault(faction.attrib.get("Name", "").lower(), {})
            for tag in ("Color", "No_Colorization_Color"):
                node = faction.find(tag)
                if node is not None and node.text:
                    row[tag] = [int(part) for part in node.text.replace(",", " ").split()[:3]]
    return colours


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CampaignSpaceCycle(unittest.TestCase):
    def test_company_created_commanders_show_on_flagship(self):
        try:
            from .campaign_space_cases import commander_campaign_arguments
        except ImportError:
            from campaign_space_cases import commander_campaign_arguments
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-commander-"))
        report = directory / "report.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--",
            *commander_campaign_arguments(support.GAME_ROOT, directory),
            "--eawr-game-root", support.GAME_ROOT, "--eawr-lighting", "sh",
            "--eawr-shadows", "on", "--eawr-environment", "map",
            "--eawr-report", str(report), "--eawr-capture", str(directory / "returned.png")],
            cwd=support.ROOT, capture_output=True, text=True, timeout=1200, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(data["space_cycle_proof"]["fight_enabled"])
        tactical = json.loads((directory / "tactical.json").read_text(encoding="utf-8"))
        riders = [row for row in tactical["live_session"]["carrier_riders"]
                  if row["type"] == "Generic_Fleet_Commander_Rebel"]
        self.assertEqual(len(riders), 2, tactical["live_session"]["carrier_riders"])
        self.assertEqual(len({row["carrier"] for row in riders}), 1)
        self.assertTrue(all(row["carrier_type"] == "Nebulon_B_Frigate" for row in riders), riders)
        self.assertTrue(all(row["reveal_raw"] * 2 == row["base_reveal_raw"] * 3 for row in riders), riders)
        self.assertIn("Generic_Fleet_Commander_Rebel", json.dumps(tactical["world_ui"]))
        self.assertTrue((directory / "space-cycle" / "tactical.png").is_file())
        print("commander GC lit capture:", directory / "space-cycle" / "tactical.png")

    def cycle(self, mode, resize=None):
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-space-cycle-"))
        report, capture = directory / "report.json", directory / "returned.png"
        began = time.monotonic()
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--",
            *qualified_campaign_arguments(mode),
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
            *(["--eawr-resize-in-loading", resize] if resize else []),
            "--eawr-report", str(report), "--eawr-capture", str(capture)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=1200, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        proof_status = data.get("space_cycle_proof", {})
        diagnostic = {key: proof_status.get(key) for key in
                      ("moved", "fight_enabled", "summary_seen", "retreat_requested", "retreat_started", "attack_tick")}
        diagnostic["input_refusals"] = len(proof_status.get("input_refusals", []))
        print(f"campaign {mode} cycle: {time.monotonic() - began:.3f}s, "
              f"{data.get('frames')} viewer frames, load={data.get('handoff', {}).get('load_ms')}ms, "
              f"proof={diagnostic}")
        phase_files = list((directory / "space-cycle").glob("*-phase.json"))
        if phase_files:
            phases = sorted((json.loads(path.read_text(encoding="utf-8")) for path in phase_files),
                            key=lambda phase: (phase["viewer_frame"], phase["steady_milliseconds"]))
            for previous, current in zip(phases, phases[1:]):
                milliseconds = current["steady_milliseconds"] - previous["steady_milliseconds"]
                print(f"  {previous['stage']} -> {current['stage']}: {milliseconds}ms, "
                      f"viewer_frame={current['viewer_frame']}, tactical_tick={current['tactical_tick']}")
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        proof = data["space_cycle_proof"]
        self.assertTrue(proof["moved"] and proof["fight_enabled"] and proof["summary_seen"])
        self.assertEqual(proof.get("input_refusals", []), [], "ordinary cycle input must reach its owner")
        self.assertTrue(proof["map"].lower().endswith(".ted"))
        required = {"galaxy", "fight", "loading", "tactical", "summary"}
        if mode == "win":
            self.assertTrue(proof["won"], "the combat branch must win through ordinary attack commands")
        if mode == "loss":
            self.assertFalse(proof["won"], "the original one-squadron attack must remain a genuine loss")
            self.assertEqual(proof["survivors"], 0)
            self.assertEqual(proof["original_members"], 1)
            self.assertEqual(proof["merge_orders"], 0)
        else:
            self.assertGreater(proof["merge_orders"], 0)
            self.assertEqual(proof["original_members"], 3)
        if mode == "retreat":
            required |= {"countdown", "cancelled"}
            self.assertTrue(proof["retreat_started"] and proof["retreat_cancelled"] and proof["retreat_restarted"])
            # HCB-15b (#3078): the trigger is the command bar's authored b_retreat slot, not a
            # top-left project button: enabled when eligible, flashing in the local countdown.
            button = proof["retreat_button"]
            self.assertTrue(button["enabled"] and button["flashing"] and button["idle_after_cancel"], button)
            self.assertIsNotNone(button["point"])
            self.assertLess(button["point"][0], 1280 / 2, button)
            self.assertGreater(button["point"][1], 720 / 2, button)
        self.assertEqual({Path(path).stem for path in proof["captures"]}, required)
        self.assertTrue(all(Path(path).is_file() for path in proof["captures"]))
        self.assertEqual(data["battle_dialog"]["phase"], "hidden")
        self.assertTrue(proof["acknowledged"])
        self.assertGreater(proof["summary_milliseconds"], 0)
        if mode != "loss":
            self.assertGreater(proof["survivors"], 0)
        self.assertTrue(proof["survivors_preserved"])
        self.assertTrue(data["handoff"]["requested"], "ordinary handoff remains reported after child teardown")
        self.assertEqual(len(data["handoff_cycles"]), 1)
        cycle = data["handoff_cycles"][0]
        self.assertTrue(cycle["child_expired"] and cycle["feed_expired"] and cycle["scene_expired"])
        self.assertTrue(cycle["galaxy_same_instance"])
        self.assertTrue(capture.is_file())
        tactical = json.loads((directory / "tactical.json").read_text(encoding="utf-8"))
        loading = tactical["loading_screen"]
        if resize:
            # #3270/#3272: the window resized while the loading screen was up; the loading screen's layout
            # and the tactical camera both follow the new viewport (edge scroll reads the camera's extent).
            width, height = (int(part) for part in resize.split("x"))
            self.assertEqual([data["battle_viewport"]["width"], data["battle_viewport"]["height"]], [width, height])
            self.assertGreaterEqual(data["battle_viewport"]["syncs"], 1)
            self.assertEqual(loading["layout_size"], [width, height])
        self.assertEqual(loading["dialog"], "IDD_SPACE_BATTLE_LOAD_DIALOG")
        self.assertTrue(loading["campaign"] and loading["planet"] and loading["owner"])
        planet = tactical["hud"]["planet_name"]
        self.assertEqual(planet["source"], "text", planet)
        self.assertTrue(planet["text_id"], planet)
        self.assertEqual(planet["context"], loading["planet"], planet)
        self.assertTrue(planet["text"] and loading["title"].endswith(planet["text"]), planet)
        self.assertGreater(tactical["hud"]["planet_pixels"], 0)
        self.assertGreaterEqual(loading["basic_income"], 0)
        # WGT-120: a campaign battle does not colorize by player; models take the type's or the faction's
        # No_Colorization_Color, and squadron frames the faction's Color (WU-21).
        live = tactical["live_session"]
        slots = {row["slot"]: row["faction"].lower() for row in live["start_slots"]}
        factions = faction_colours()
        rendered = [row for row in live["rendered_colours"] if row["player"] in slots]
        self.assertTrue(rendered, live["rendered_colours"])
        for row in rendered:
            self.assertIn(row["source"], {"type", "faction"}, row)
            if row["source"] == "faction":
                self.assertEqual(row["rgb"], factions[slots[row["player"]]]["No_Colorization_Color"], row)
        frames = [row for row in tactical["world_ui"]["identity_geometry"] if row["owner"] in slots]
        print(f"campaign squadron frames checked: {len(frames)}")
        for row in frames:
            self.assertEqual(row["tint"], factions[slots[row["owner"]]]["Color"], row)
        # #3150 (WBP-52): a pad's capture bar and model tint take the model colour (type, else faction), never
        # the lobby colour a campaign battle lacks (which left them white).
        pads = []
        for pad in live["pads"]:
            player = next((value for value in (pad["target"], pad["owner"]) if value in slots), None)
            if player is None:
                continue
            pads.append(pad)
            self.assertIn("capture_rgb", pad, pad)
            expected = factions[slots[player]]["No_Colorization_Color"]
            self.assertEqual(pad["capture_rgb"], expected, pad)
        print(f"campaign pads checked: {len(pads)} of {len(live['pads'])}")

    @unittest.skip("Pending qualified stock winning-matchup GPU acceptance (legacy EAWR-2645)")
    def test_viewer_gc_space_win(self):
        self.cycle("win")

    def test_viewer_gc_space_retreat(self):
        self.cycle("retreat")

    def test_viewer_gc_space_loss(self):
        self.cycle("loss")

    def test_viewer_gc_space_resize_in_loading(self):
        self.cycle("loss", resize="1600x900")


if __name__ == "__main__":
    unittest.main()
