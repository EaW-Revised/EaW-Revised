import unittest

from bridge import Parser, Unsupported, bridge


VS = """#version 330
layout(binding=0,std140) uniform Constants { vec4 rows[4]; } transform;
layout(location=0) in vec4 position;
out vec2 uv;
void main() {
gl_Position = vec4(dot(position, transform.rows[0u]), position.yzw);
uv = position.xy;
}
"""
PS = """uniform sampler2D colour;
in vec4 texcoord;
out vec4 result;
void main() { result = texture(colour, texcoord.xy) * vec4(0.5); }
"""
VR = {"inputs": [{"name": "position", "type": "vec4", "location": 0}],
      "outputs": [{"name": "uv", "type": "vec2", "location": 2}]}
PR = {"inputs": [{"name": "texcoord", "type": "vec4", "location": 2}],
      "outputs": [{"name": "result", "type": "vec4", "location": 0}]}
POLICY = {"vs": {"inputs": {"position": "vec4(VERTEX,1.0)"}},
          "ps": {"samplers": {"colour": "BaseTexture"}}}


class BridgeContracts(unittest.TestCase):
    def test_links_by_location_and_pads_missing_channels(self):
        text = bridge(VS, PS, VR, PR, POLICY)
        self.assertIn("texcoord = vec4(gate_link_2, 0.0, 1.0);", text)
        self.assertIn("uniform vec4 vs_transform_rows_0;", text)
        self.assertNotIn("std140", text)
        self.assertNotIn("#version", text)
        self.assertIn("POSITION = gate_position;", text)
        self.assertIn("ALBEDO = result.rgb;", text)

    def test_missing_varying_and_sampler_fail_closed(self):
        with self.assertRaises(Unsupported):
            bridge(VS, PS, {**VR, "outputs": []}, PR, POLICY)
        with self.assertRaises(Unsupported):
            bridge(VS, PS, VR, PR, {**POLICY, "ps": {}})

    def test_dynamic_or_out_of_range_constants_fail_closed(self):
        for index in ("position.x", "4u"):
            with self.assertRaises(Unsupported):
                bridge(VS.replace("[0u]", f"[{index}]"), PS, VR, PR, POLICY)

    def test_control_flow_and_unknown_calls_fail_closed(self):
        for statement in ("for(int i=0;i<4;i++) {}", "result = arbitrary(1.0);", "discard;"):
            with self.assertRaises(Unsupported):
                Parser("void main() {" + statement + "}").parse()

    def test_unterminated_or_unknown_syntax_fails(self):
        for source in ("layout(", "void main() { @ }", "uniform image2D image; void main() {}"):
            with self.assertRaises((Unsupported, IndexError)):
                Parser(source).parse()


if __name__ == "__main__":
    unittest.main()
