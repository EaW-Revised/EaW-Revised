"""Land compose texture chain structure (MT-02, legacy #6723)."""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class LandTextureChainStructure(unittest.TestCase):
    def test_land_placement_textures_complete_the_mip_chain(self):
        # MT-02 (docs/behaviour/texture-mip-upload.md): a single-level mod texture of a placed unit, building or prop
        # must not reach the GPU as one level. One helper loads it and applies the chain policy; every texture the land
        # compose loads goes through it, so a raw assets::load_texture in this file fails here.
        text = (ROOT / "apps/viewer/src/map_mode_placements.cpp").read_text(encoding="utf-8")
        self.assertEqual(text.count("assets::load_texture("), 1,
                         "land compose loads a texture outside load_placement_texture")
        helper = text[text.index("load_placement_texture(\n"):]
        helper = helper[:helper.index("\n}\n")]
        self.assertIn("assets::load_texture(", helper)
        self.assertLess(helper.index("assets::load_texture("), helper.index("apply_galactic_chain("))
        # The base texture follows the space policy (effect phase, never file or mod name); the family normal maps get
        # the normal chain; the held copy is keyed by path and policy so one file wanted by two effects is two entries.
        self.assertIn("texture_processing_for(*selector)", text)
        self.assertIn("normal_binding_names(*selector)", text)
        self.assertIn("TextureProcessing::normal_mip_chain", text)
        self.assertEqual(text.count("load_placement_texture(*filesystem"), 2)
        self.assertEqual(text.count("ComposeTextureCache::key("), 2)


if __name__ == "__main__":
    unittest.main()
