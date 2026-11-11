"""Execute production prewarm array setup against the CPU mesh-array contract."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'src/presentation/godot/renderer_materials.cpp'
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain


HARNESS = r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <variant>
#include <vector>
struct Vector3 { float x, y, z; Vector3(float a, float b, float c): x(a), y(b), z(c) {} };
struct Vector2 { float x{}, y{}; };
struct Color { float r, g, b, a; Color(float w, float x, float y, float z): r(w), g(x), b(y), a(z) {} };
template<class T> struct Packed : std::vector<T> {
    using std::vector<T>::vector;
    T* ptrw() { return this->data(); }
};
using PackedVector3Array = Packed<Vector3>;
using PackedVector2Array = Packed<Vector2>;
using PackedColorArray = Packed<Color>;
using PackedFloat32Array = Packed<float>;
using PackedByteArray = Packed<std::uint8_t>;
using PackedInt32Array = Packed<std::int32_t>;
using Value = std::variant<std::monostate, PackedVector3Array, PackedVector2Array,
    PackedColorArray, PackedFloat32Array, PackedByteArray, PackedInt32Array>;
struct Array : std::vector<Value> {};
struct RID { unsigned id{}; bool is_valid() const { return id != 0; } };
template<class T> struct BitField {
    std::int64_t value;
    explicit BitField(std::int64_t bits): value(bits) {}
};
struct RenderingServer {
    enum ArrayType { ARRAY_VERTEX, ARRAY_NORMAL, ARRAY_TANGENT, ARRAY_COLOR,
        ARRAY_TEX_UV, ARRAY_TEX_UV2, ARRAY_CUSTOM0, ARRAY_CUSTOM1, ARRAY_CUSTOM2,
        ARRAY_CUSTOM3, ARRAY_BONES, ARRAY_WEIGHTS, ARRAY_INDEX, ARRAY_MAX };
    enum ArrayFormat { ARRAY_CUSTOM_RGB_FLOAT = 6, ARRAY_FORMAT_CUSTOM0_SHIFT = 13 };
    enum { PRIMITIVE_TRIANGLES = 3 };
    struct Upload { int mesh; bool accepted; bool floats; std::size_t elements; };
    std::vector<Upload> uploads;
    std::array<bool, 10> live{};
    unsigned invalid_frees{};
    // References established by production prewarm: child slot -> parent slot.
    // A referenced object must outlive its dependents, including a viewport's
    // scenario and camera and both mesh instances' materials through the meshes.
    void free_rid(RID rid) {
        const auto slot = rid.id - 1;
        if (!live[slot]) ++invalid_frees;
        for (const auto& edge : std::array<std::array<unsigned, 2>, 10>{{
            {1,0}, {2,1}, {7,1}, {6,2}, {5,3}, {6,3}, {8,3}, {5,4}, {8,7}, {8,9}}})
            if (edge[1] == slot && live[edge[0]]) ++invalid_frees;
        live[slot] = false;
    }
    // Independent boundary rule: Mesh ARRAY_CUSTOM0 is slot 6; RGB_FLOAT
    // requires three float elements per vertex, never an opaque byte array.
    static bool accepts(const Array& arrays, BitField<ArrayFormat> format) {
        if (arrays.size() != ARRAY_MAX) return false;
        const auto* vertices = std::get_if<PackedVector3Array>(&arrays[ARRAY_VERTEX]);
        const auto* custom = std::get_if<PackedFloat32Array>(&arrays[ARRAY_CUSTOM0]);
        const auto custom_format = (format.value >> ARRAY_FORMAT_CUSTOM0_SHIFT) & 7;
        if (!vertices || !custom || custom_format != ARRAY_CUSTOM_RGB_FLOAT
            || custom->size() != vertices->size() * 3) return false;
        const auto* bones = std::get_if<PackedInt32Array>(&arrays[ARRAY_BONES]);
        const auto* weights = std::get_if<PackedFloat32Array>(&arrays[ARRAY_WEIGHTS]);
        return (!bones && !weights) || (bones && weights
            && bones->size() == vertices->size() * 4 && weights->size() == vertices->size() * 4);
    }
    void mesh_add_surface_from_arrays(int mesh, int, const Array& arrays, int, int,
        BitField<ArrayFormat> format) {
        const bool floats = std::holds_alternative<PackedFloat32Array>(arrays[ARRAY_CUSTOM0]);
        const auto elements = std::visit([](const auto& value) -> std::size_t {
            if constexpr (requires { value.size(); }) return value.size();
            else return 0;
        }, arrays[ARRAY_CUSTOM0]);
        uploads.push_back({mesh, accepts(arrays, format), floats, elements});
    }
};
__PRODUCTION_BUILDERS__
__PRODUCTION_TEMPORARY__
int main() {
    RenderingServer server;
    auto* rendering = &server;
    const int mesh = 0, skinned_mesh = 1;
    // The entire setup and both upload calls below are extracted verbatim.
    __PRODUCTION__
    auto wrong_type = arrays;
    wrong_type[RenderingServer::ARRAY_CUSTOM0] = PackedByteArray(36);
    auto wrong_count = arrays;
    wrong_count[RenderingServer::ARRAY_CUSTOM0] = PackedFloat32Array(36);
    std::cout << std::boolalpha << "{\"uploads\":[";
    bool first = true;
    for (const auto& call : server.uploads) {
        std::cout << (first ? "" : ",") << "{\"mesh\":" << call.mesh
            << ",\"accepted\":" << call.accepted << ",\"floats\":" << call.floats
            << ",\"elements\":" << call.elements << "}";
        first = false;
    }
    unsigned cleanup_errors = 0, cleanup_leaks = 0;
    for (unsigned mask = 0; mask < 1024; ++mask) {
        RenderingServer cleanup;
        {
            Temporary owned{cleanup, {}};
            for (unsigned slot = 0; slot < 10; ++slot) if (mask & (1U << slot)) {
                cleanup.live[slot] = true;
                owned.rids[slot].id = slot + 1;
            }
        }
        cleanup_errors += cleanup.invalid_frees;
        for (const bool alive : cleanup.live) cleanup_leaks += alive ? 1U : 0U;
    }
    std::cout << "],\"wrong_type_accepted\":" << RenderingServer::accepts(wrong_type, format)
        << ",\"wrong_count_accepted\":" << RenderingServer::accepts(wrong_count, format)
        << ",\"allocation_subsets\":1024,\"cleanup_errors\":" << cleanup_errors
        << ",\"cleanup_leaks\":" << cleanup_leaks << "}";
}
'''


class PrewarmMeshLayoutContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = SOURCE.read_text(encoding='utf-8')
        method = source.split('GodotRenderer::prewarm_shader_material(', 1)[1]
        production_temporary = 'struct Temporary final {' + method.split('struct Temporary final {', 1)[1].split(
            '} temporary{', 1)[0] + '};'
        if 'Array prewarm_mesh_arrays(' in source:
            builder_start = source.index('Array prewarm_mesh_arrays(')
            builder_end = source.index('core::Result<std::shared_ptr<const shader_activation::Artifact>>', builder_start)
            builders = source[builder_start:builder_end]
            start = method.index('    const auto format = prewarm_mesh_format();')
        else:
            # Retain the pre-extraction source as an executable regression
            # baseline: exactly the same setup and upload boundary is tested.
            builders = ''
            start = method.index('    Array arrays;')
        end = method.index('    if (rendering->mesh_get_surface_count', start)
        production = method[start:end]
        toolchain = select_toolchain()
        if toolchain is None:
            raise unittest.SkipTest('requires a C++ compiler on the validation host')
        with tempfile.TemporaryDirectory(prefix='eawr-prewarm-layout-') as temporary:
            directory = Path(temporary)
            cpp, executable = directory / 'layout.cpp', directory / 'layout.exe'
            cpp.write_text(HARNESS.replace('__PRODUCTION_BUILDERS__', builders)
                           .replace('__PRODUCTION_TEMPORARY__', production_temporary)
                           .replace('__PRODUCTION__', production), encoding='utf-8')
            compile_fixture(toolchain, cpp, executable)
            completed = subprocess.run([str(executable)], check=True, capture_output=True,
                                       text=True, timeout=10)
            cls.evidence = json.loads(completed.stdout)

    def test_rigid_and_skinned_prewarm_arrays_match_declared_custom_format(self):
        self.assertEqual([row['mesh'] for row in self.evidence['uploads']], [0, 1])
        for upload in self.evidence['uploads']:
            with self.subTest(mesh=upload['mesh']):
                self.assertTrue(upload['accepted'], 'CUSTOM0 requires three float elements per vertex')
                self.assertTrue(upload['floats'])
                self.assertEqual(upload['elements'], 9)

    def test_boundary_rejects_bytes_and_byte_count_used_as_float_count(self):
        self.assertFalse(self.evidence['wrong_type_accepted'])
        self.assertFalse(self.evidence['wrong_count_accepted'])

    def test_production_cleanup_keeps_dependencies_alive_for_all_allocation_subsets(self):
        self.assertEqual(self.evidence['allocation_subsets'], 1024)
        self.assertEqual(self.evidence['cleanup_errors'], 0)
        self.assertEqual(self.evidence['cleanup_leaks'], 0)


if __name__ == '__main__':
    unittest.main()
