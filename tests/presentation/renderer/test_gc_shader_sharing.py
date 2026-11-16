"""Structural contract: one shader cache per campaign session, used by every renderer and battle of it.

The GPU side (programs outliving renderers and the cache's creator) runs in the renderer runtime exercise
(test_runtime_renderer.py); this guards the wiring that is only reachable in a campaign session.
"""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]


def text(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


class GalacticShaderSharingContracts(unittest.TestCase):
    def test_every_galactic_renderer_is_built_on_the_session_cache(self):
        for path in sorted((ROOT / "apps/viewer/src/galactic").glob("*.cpp")):
            source = path.read_text(encoding="utf-8")
            # Whitespace-tolerant: constructions may span lines or use make_shared / direct construction.
            constructions = re.finditer(r"(?:make_unique|make_shared)\s*<\s*GodotRenderer\s*>\s*\(([^;]*)\)\s*;", source)
            for match in constructions:
                self.assertIn("shaders", match.group(1), f"{path.name}: {match.group(0)}")
            self.assertNotRegex(source, r"\bGodotRenderer\s+\w+\s*[({]", path.name)

    def test_session_cache_is_declared_before_the_renderers_that_hold_it(self):
        header = text("apps/viewer/src/galactic/galactic_mode_internal.hpp")
        cache = header.index("std::shared_ptr<GodotShaderCache> shaders = std::make_shared<GodotShaderCache>();")
        self.assertLess(cache, header.index("std::unique_ptr<GodotRenderer> renderer;"))
        self.assertLess(cache, header.index("std::unique_ptr<GodotRenderer> capture_renderer;"))

    def test_every_battle_handoff_gives_the_battle_the_session_cache(self):
        source = text("apps/viewer/src/galactic/galactic_handoff.cpp")
        options = [m.start() for m in re.finditer(r"MapMode::Options map;", source)]
        self.assertGreaterEqual(len(options), 2)
        for start in options:
            self.assertIn("map.shaders = shaders;", source[start:start + 2500])

    def test_programs_enter_and_leave_the_cache_through_one_admission_rule(self):
        for relative in ("src/presentation/godot/renderer_materials.cpp", "src/presentation/godot/renderer_fog.cpp"):
            source = text(relative)
            self.assertNotIn("entries.emplace", source, relative)
            self.assertNotIn("entries.find", source, relative)
            self.assertIn("shader_cache_->impl_->find(", source, relative)
            self.assertIn("shader_cache_->impl_->admit(", source, relative)

    def test_fog_variant_shares_and_detach_frees_only_what_it_owns(self):
        source = text("src/presentation/godot/renderer_fog.cpp")
        detach = source.split("void GodotRenderer::Impl::detach_fog_consumer", 1)[1].split("\n}\n", 1)[0]
        self.assertRegex(detach, r"if \(!consumer\.shared_shader\)\s*render_churn::free_rid\(\*rendering, consumer\.shader")
        self.assertIn("consumer.shared_shader.reset();", detach)
        # Only a variant that passed both admission checks is cached, so a hit can skip them.
        attach = source.split("GodotRenderer::Impl::attach_fog_consumer", 1)[1]
        self.assertLess(attach.index("portable_limit_problem(uniforms, tokens)"), attach.index("impl_->admit(source"))
        # A hit may be a program another route admitted (the key is the source text): the fog uniform
        # contract is checked on the hit as well, before any material uses it.
        hit = attach.split("if (consumer.shared_shader) {", 1)[1].split("} else {", 1)[0]
        self.assertIn("fog_uniform_problem(*rendering, consumer.shader)", hit)
        self.assertIn("consumer.shared_shader.reset();", hit)


if __name__ == "__main__":
    unittest.main()
