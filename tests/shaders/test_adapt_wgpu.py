import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).parents[2] / "tools" / "shaders" / "adapt_wgpu.py"
SPEC = importlib.util.spec_from_file_location("adapt_wgpu", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(MODULE)


class AdaptWgpuTests(unittest.TestCase):
    def test_vertex_only_changes_output_position_semantic(self):
        source = "struct VS_INPUT { float4 Pos : POSITION; };\nstruct VS_OUTPUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; };"
        adapted = MODULE.adapt_vertex(source)
        self.assertIn("VS_INPUT { float4 Pos : POSITION;", adapted)
        self.assertIn("float4 Pos : SV_POSITION;", adapted)

    def test_fragment_splits_combined_sampler_without_changing_equation(self):
        source = "struct VS_OUTPUT { float4 Pos : POSITION; };\nsampler2D BaseSampler;\nfloat4 f(float2 uv) { return tex2D(BaseSampler, uv) * 2.0; }"
        adapted = MODULE.adapt_fragment(source)
        self.assertIn("vk::binding(1, 0)", adapted)
        self.assertIn("vk::binding(2, 0)", adapted)
        self.assertIn("BaseTexture.Sample(BaseSamplerState, uv) * 2.0", adapted)

    def test_fail_closed_when_expected_contract_is_absent(self):
        with self.assertRaises(ValueError):
            MODULE.adapt_vertex("float4 main() : POSITION { return 0; }")
        with self.assertRaises(ValueError):
            MODULE.adapt_fragment("Texture2D already_modern;")


if __name__ == "__main__":
    unittest.main()
