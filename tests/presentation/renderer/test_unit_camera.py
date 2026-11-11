"""Execute unit camera option parsing and selection without a graphics device."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain


HARNESS = r'''
#include "eawr/presentation/space/space.hpp"
#include "__MODEL_PREVIEW_CPP__"
#include <cassert>
#include <filesystem>
#include <iostream>
namespace eawr::presentation::space {
__CAMERA_PARSER__
}
using eawr::presentation::FixedCamera;
namespace space = eawr::presentation::space;
namespace preview = eawr::viewer::model_preview;
namespace viewer = eawr::viewer;
struct PackedStringArray {
    std::vector<std::string> values;
    int64_t size() const { return static_cast<int64_t>(values.size()); }
    const std::string& operator[](int64_t index) const { return values[static_cast<std::size_t>(index)]; }
};
PackedStringArray arguments;
PackedStringArray viewer_profile_arguments() { return arguments; }
std::string utf8(const std::string& value) { return value; }
struct ViewerPath {
    std::string value;
    std::filesystem::path native() const { return value; }
};
struct UnitMode {
    __OPTIONS__
    static Options from_command_line();
};
__COMMAND_LINE__
struct Selection {
    UnitMode::Options options;
    preview::Bounds bounds;
    FixedCamera base_camera;
    std::array<float, 3> view{0.8F, 0.5F, 1.0F};
    FixedCamera camera;
    std::string failure;
    bool fail(std::string text) { failure = std::move(text); return false; }
    bool select() {
        __SELECTION__
        return true;
    }
};
bool same_camera(const FixedCamera& a, const FixedCamera& b) {
    return a.eye == b.eye && a.target == b.target && a.up == b.up
        && a.vertical_fov_degrees == b.vertical_fov_degrees
        && a.near_plane == b.near_plane && a.far_plane == b.far_plane
        && a.width == b.width && a.height == b.height;
}
int main() {
    const std::string explicit_camera = "300,240,400,0,50,0,0,1,0,45,1,2000";
    arguments.values = {"--eawr-unit", "model.alo", "--eawr-unit-times", "0"};
    const auto defaults = UnitMode::from_command_line();
    assert(!defaults.camera && defaults.view == "0.8,0.5,1.0");
    assert(defaults.times == "0" && defaults.model_path == "model.alo");
    Selection fitted;
    fitted.options = defaults;
    fitted.bounds = {{-80,-80,0}, {80,80,80}};
    fitted.base_camera.width = 960; fitted.base_camera.height = 540;
    const std::array<float, 16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto expected = preview::fit_camera(preview::world_bounds(fitted.bounds, identity),
        fitted.base_camera, fitted.view, 1.15F);
    assert(expected && fitted.select() && same_camera(fitted.camera, expected.value->camera));

    arguments.values = {"--eawr-unit", "model.alo", "--eawr-unit-camera", explicit_camera,
                        "--eawr-unit-times", "0"};
    const auto pinned = UnitMode::from_command_line();
    assert(pinned.camera && *pinned.camera == explicit_camera && pinned.times == "0");
    const auto parsed = space::parse_camera(explicit_camera, 960, 540);
    assert(parsed.status == space::CameraStatus::valid);
    assert((parsed.camera.eye == std::array<float, 3>{300,240,400}));
    assert((parsed.camera.target == std::array<float, 3>{0,50,0}));
    assert((parsed.camera.up == std::array<float, 3>{0,1,0}));
    assert(parsed.camera.vertical_fov_degrees == 45 && parsed.camera.near_plane == 1
        && parsed.camera.far_plane == 2000);
    unsigned fixed_bounds = 0;
    for (const float extent : {1.0F, 80.0F, 100.0F, 10000.0F}) {
        Selection fixed;
        fixed.options = pinned;
        fixed.base_camera = fitted.base_camera;
        fixed.bounds = {{-extent,-extent,0}, {extent,extent,extent}};
        assert(fixed.select() && same_camera(fixed.camera, parsed.camera));
        ++fixed_bounds;
    }
    Selection invalid_bounds;
    invalid_bounds.options = pinned;
    invalid_bounds.base_camera = fitted.base_camera;
    invalid_bounds.bounds = {{1,1,1}, {-1,-1,-1}};
    assert(invalid_bounds.select() && same_camera(invalid_bounds.camera, parsed.camera));
    unsigned invalid_inputs = 0;
    for (const std::string& text : std::vector<std::string>{"", "1,2,3",
             "nan,240,400,0,50,0,0,1,0,45,1,2000",
             "300,240,400,0,50,0,0,1,0,0,1,2000",
             "300,240,400,0,50,0,0,1,0,45,2000,1",
             "0,0,0,0,0,0,0,1,0,45,1,2000",
             "0,0,1,0,0,0,0,0,1,45,1,2000"}) {
        arguments.values = {"--eawr-unit-camera", text};
        Selection invalid;
        invalid.options = UnitMode::from_command_line();
        assert(invalid.options.camera);
        invalid.bounds = fitted.bounds;
        assert(!invalid.select() && invalid.failure.starts_with("--eawr-unit-camera: "));
        ++invalid_inputs;
    }
    std::cout << "{\"default_matches_existing_fit\":true,\"fixed_bounds_cases\":" << fixed_bounds
        << ",\"explicit_bypasses_invalid_bounds\":true,\"invalid_inputs_rejected\":" << invalid_inputs
        << ",\"option_round_trip\":true}";
}
'''


class UnitCameraContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        toolchain = select_toolchain()
        if toolchain is None:
            raise unittest.SkipTest('requires a C++ compiler on the validation host')
        source = (ROOT / 'apps/viewer/src/unit_mode.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'apps/viewer/src/unit_mode.hpp').read_text(encoding='utf-8')
        options = 'struct Options final {' + header.split('struct Options final {', 1)[1].split('\n    };', 1)[0] + '\n};'
        command_line = 'UnitMode::Options UnitMode::from_command_line() {' + source.split(
            'UnitMode::Options UnitMode::from_command_line() {', 1)[1].split('\nUnitMode::UnitMode(', 1)[0]
        selection = '    if (options.camera) {' + source.split('    if (options.camera) {', 1)[1].split(
            '    layout = preview::strip_layout(', 1)[0]
        geometry = (ROOT / 'src/presentation/space/space_geometry.cpp').read_text(encoding='utf-8')
        parser = geometry.split('namespace eawr::presentation::space {', 1)[1].split(
            '// -- mode-7 sun reference geometry', 1)[0]
        harness = HARNESS.replace('__MODEL_PREVIEW_CPP__', (ROOT / 'apps/viewer/src/model_preview.cpp').as_posix())
        for key, value in [('OPTIONS', options), ('COMMAND_LINE', command_line),
                           ('SELECTION', selection), ('CAMERA_PARSER', parser)]:
            harness = harness.replace('__' + key + '__', value)
        with tempfile.TemporaryDirectory(prefix='eawr-unit-camera-') as temporary:
            directory = Path(temporary)
            cpp, executable = directory / 'camera.cpp', directory / 'camera.exe'
            cpp.write_text(harness, encoding='utf-8')
            compile_fixture(toolchain, cpp, executable, include_dirs=(ROOT / 'include',))
            completed = subprocess.run([str(executable)], capture_output=True, text=True, check=True, timeout=10)
            cls.evidence = json.loads(completed.stdout)

    def test_default_camera_matches_the_existing_bounds_fit(self):
        self.assertTrue(self.evidence['default_matches_existing_fit'])

    def test_explicit_camera_round_trips_and_ignores_bounds(self):
        self.assertTrue(self.evidence['option_round_trip'])
        self.assertEqual(self.evidence['fixed_bounds_cases'], 4)
        self.assertTrue(self.evidence['explicit_bypasses_invalid_bounds'])

    def test_malformed_and_invalid_explicit_cameras_fail_closed(self):
        self.assertEqual(self.evidence['invalid_inputs_rejected'], 7)


if __name__ == '__main__':
    unittest.main()
