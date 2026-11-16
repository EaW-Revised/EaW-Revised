"""Land water texture chain and terrain blend layer structure (MT-02, legacy #6764)."""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class LandWaterChainStructure(unittest.TestCase):
    def test_water_lookup_completes_the_mip_chain_on_the_loading_thread(self):
        # The water plane and the river / track / water-decoration textures are sampled with linear mip filtering, so a
        # single-level mod texture must not reach the GPU as one level. compose_water gets a lookup that wraps the plain
        # one and completes the chain inside a load-profile scope; it must not be handed the plain lookup.
        text = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        start = text.index("water_lookup_texture = [&lookup_texture]")
        lookup = text[start:text.index("\n            };\n", start)]
        self.assertLess(lookup.index("lookup_texture(declared)"), lookup.index("assets::extend_mip_chain("))
        self.assertIn("assets::extend_mip_chain(*texture, assets::ChainKind::colour)", lookup)
        self.assertIn("core::load_profile::Phase::texture", lookup)
        self.assertRegex(text, r"compose_water\(\*state\.renderer, map, mesh, water_lookup_texture,")

    def test_terrain_blend_layers_are_mipped_by_the_renderer(self):
        # The terrain blend layers keep the plain lookup on purpose: the shared texture array takes the top level only,
        # resizes every layer to one edge and regenerates the whole chain, so no chain work belongs on the loading thread.
        ready = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        self.assertRegex(ready, r"register_terrain_blend\(\*state\.renderer, blend\.value\(\),\s*map\.terrain->materials, lookup_texture,")
        upload = (ROOT / "src/presentation/godot/renderer_mesh_upload.cpp").read_text(encoding="utf-8")
        start = upload.index("::register_shared_texture_array(")
        body = upload[start:upload.index("\n}\n", start)]
        self.assertIn("layer.mips.front()", body)
        self.assertIn("image->resize(", body)
        self.assertIn("image->generate_mipmaps()", body)
        shaders = (ROOT / "apps/viewer/src/land_look_shaders.hpp").read_text(encoding="utf-8")
        self.assertIn("uniform sampler2DArray eawr_layers : filter_linear_mipmap", shaders)


if __name__ == "__main__":
    unittest.main()
