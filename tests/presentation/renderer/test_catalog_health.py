"""Viewer setup reports active catalog failures before trying to build the lobby."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"), "requires the viewer GPU lane")
class CatalogHealthGpu(unittest.TestCase):
    def test_setup_catalog_health(self):
        # Invented data needs no retail assets: setup must act on the catalog before
        # reading lobby constants, maps or textures (R-10, R-13, C-06).
        with tempfile.TemporaryDirectory(prefix="catalog-health-viewer-") as temporary:
            base = Path(temporary)
            for variant in ("malformed", "wrong-root", "missing"):
                with self.subTest(variant=variant):
                    install = base / variant
                    for layer in ("GameData", "corruption"):
                        data = install / layer / "Data"
                        (data / "XML").mkdir(parents=True)
                        (data / "MegaFiles.xml").write_text(
                            "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
                    xml = install / "GameData" / "Data" / "XML"
                    for filename, tag in (("GameObjectFiles.xml", "Game_Object_Files"),
                                          ("FactionFiles.xml", "Faction_Files"),
                                          ("CampaignFiles.xml", "Campaign_Files"),
                                          ("SFXEventFiles.xml", "SFXEvent_Files")):
                        (xml / filename).write_text(f"<{tag}/>", encoding="utf-8")
                    (xml / "HardpointDataFiles.xml").write_text(
                        "<Hard_Point_Files><File>health.xml</File></Hard_Point_Files>", encoding="utf-8")
                    if variant != "missing":
                        (xml / "health.xml").write_text(
                            "<HardPoints><HardPoint></HardPoints>" if variant == "malformed" else "<HardPoints/>",
                            encoding="utf-8")
                    if variant == "wrong-root":
                        (xml / "FactionFiles.xml").write_text("<Wrong_Root/>", encoding="utf-8")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--quit-after", "5",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                        "--eawr-game-root", str(install), "--eawr-audio", "off"],
                        cwd=ROOT, capture_output=True, text=True, timeout=120)
                    output = result.stdout + result.stderr
                    (base / (variant + ".log")).write_text(output, encoding="utf-8")
                    if variant == "malformed":
                        self.assertIn("EAWR-XML-0001", output)
                        self.assertIn("health.xml", output)
                    elif variant == "wrong-root":
                        self.assertIn("EAWR-XML-0003", output)
                    else:
                        self.assertIn("EAWR catalog load: include_missing=1", output)
                        self.assertNotIn("EAWR-XML-0001", output)
                        self.assertNotIn("EAWR-XML-0003", output)
                    self.assertNotIn("EAWR skirmish setup ready", output)


if __name__ == "__main__":
    unittest.main()
