"""Overlay preservation and report-integrity contracts for the live capture tools."""
import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import xml.etree.ElementTree as ET

from tools.capture.gc_ai_proof import capture_timeout, collect, make_spec, overlay_from_sources, validate_report


class GalacticAiProofTools(unittest.TestCase):
    def test_renamed_campaign_preserves_ai_and_authored_references(self):
        xml = b'<Campaigns><Campaign Name="Renamed"><Locations>A,B</Locations><Starting_Active_Player>Side_H</Starting_Active_Player><AI_Player_Control>Side_X, Mod_Controller</AI_Player_Control></Campaign><Campaign Name="Other"><Locations>B,A</Locations><AI_Player_Control>Side_H, Renamed_Controller</AI_Player_Control></Campaign></Campaigns>'
        source = SimpleNamespace(logical_path="data/xml/custom-campaign.xml", data=xml)
        with tempfile.TemporaryDirectory() as directory:
            overlay = overlay_from_sources([source], "renamed", directory)
            campaign = ET.parse(overlay / source.logical_path).getroot()[0]
            self.assertEqual(campaign.findtext("AI_Player_Control"), "Side_X, Mod_Controller")
            self.assertEqual(campaign.findtext("Locations"), "A,B")
            self.assertIn("Side_H, Renamed_Controller", [x.text for x in campaign.findall("AI_Player_Control")])
            self.assertIsNone(campaign.find("Planet_Auto_Reveal"))
            self.assertEqual(source.data, xml)
            self.assertTrue(ET.parse(overlay / "Data" / "MegaFiles.xml").getroot().findtext("File"))
            spec = make_spec("Renamed", overlay, 5400, 37)
            self.assertEqual(len(spec["runs"]), 2)
            self.assertEqual(spec["runs"][0]["args"], spec["runs"][1]["args"])
            self.assertNotIn("Machine", spec)
            with self.assertRaises(ValueError):
                overlay_from_sources([source, source], "Renamed", directory)
            with self.assertRaises(ValueError):
                overlay_from_sources([SimpleNamespace(logical_path="../escaped.xml", data=xml)], "Renamed", directory)

    def test_authored_reveal_and_other_campaign_values_are_preserved(self):
        for reveal in ("False", "Yes"):
            xml = f'<Campaigns><Campaign Name="Renamed"><Planet_Auto_Reveal>{reveal}</Planet_Auto_Reveal><Mod_Tag>custom</Mod_Tag></Campaign></Campaigns>'.encode()
            source = SimpleNamespace(logical_path="data/xml/custom.xml", data=xml)
            with tempfile.TemporaryDirectory() as directory:
                overlay = overlay_from_sources([source], "Renamed", directory)
                self.assertEqual(ET.tostring(ET.parse(overlay / source.logical_path).getroot()), ET.tostring(ET.fromstring(xml)))

    def test_phase_two_timeout_covers_the_whole_job(self):
        self.assertEqual(capture_timeout(make_spec("Renamed", "overlay", 5400)), 4800)
        self.assertEqual(capture_timeout(make_spec("Renamed", "overlay", 54000)), 37200)

    def test_missing_samples_and_different_metrics_are_refused(self):
        proof = {"all_ai": True, "event_feed_complete": True, "end_state_hash": "abc",
                 "samples": [{"galactic_frame": n, "players": []} for n in (0, 1800, 3600, 5400)]}
        data = {"status": "captured", "ai_proof": proof, "proof_capture_frames": [1800, 3600, 5400],
                "ai_proof_factions": [], "campaign": "Renamed", "ai_proof_seed": 37}
        validate_report(data, 5400)
        bad = copy.deepcopy(data); bad["ai_proof"]["samples"].pop(1)
        with self.assertRaisesRegex(ValueError, "missing"):
            validate_report(bad, 5400)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in ("a", "b"):
                (directory / f"repeat-{name}.json").write_text(json.dumps(data), encoding="utf-8")
                for n in (1800, 3600, 5400):
                    (directory / f"repeat-{name}-frame-{n}.png").write_bytes(b'x' * 101)
            result = collect(directory, directory / "result", 5400)
            self.assertTrue(result["deterministic_repeats"])
            bad = copy.deepcopy(data); bad["ai_proof"]["samples"][-1]["players"] = [{"fleet_moves": 1}]
            (directory / "repeat-b.json").write_text(json.dumps(bad), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differ"):
                collect(directory, directory / "result", 5400)
            (directory / "repeat-b.json").write_text(json.dumps(data), encoding="utf-8")
            (directory / "repeat-a-frame-1800.png").unlink()
            with self.assertRaisesRegex(ValueError, "missing galaxy-map"):
                collect(directory, directory / "result", 5400)


if __name__ == "__main__":
    unittest.main()
