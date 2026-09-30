"""Per-GPU expectation helpers (tests/presentation/gpu_expectations.py)."""

import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gpu_expectations as gpu  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None


class ProfileTests(unittest.TestCase):
    def probe(self, table, env):
        class Probe(unittest.TestCase):
            def runTest(inner):
                inner.value = gpu.expected_for_gpu(inner, table, "capture hash")
        case = Probe()
        with mock.patch.dict(os.environ, env, clear=False):
            if not env.get(gpu.PROFILE_ENV):
                os.environ.pop(gpu.PROFILE_ENV, None)
            result = unittest.TestResult()
            case.run(result)
        return case, result

    def test_the_profile_entry_is_returned(self):
        case, result = self.probe({"nvidia-gtx970": "a", "amd-rx7900xtx": "b"}, {gpu.PROFILE_ENV: "nvidia-gtx970"})
        self.assertEqual((case.value, result.skipped), ("a", []))

    def test_a_missing_entry_skips_with_the_reason(self):
        _, result = self.probe({"amd-rx7900xtx": "b"}, {gpu.PROFILE_ENV: "nvidia-gtx970"})
        self.assertIn("no capture hash recorded for GPU profile nvidia-gtx970", result.skipped[0][1])

    def test_no_profile_skips(self):
        _, result = self.probe({"amd-rx7900xtx": "b"}, {})
        self.assertIn("set EAWR_GPU_PROFILE", result.skipped[0][1])

    def test_unknown_profiles_in_a_table_are_an_error(self):
        _, result = self.probe({"nvidia-gtx980": "x"}, {gpu.PROFILE_ENV: "nvidia-gtx970"})
        self.assertEqual(len(result.errors), 1)


@unittest.skipIf(Image is None, "Pillow is not installed")
class ImageTests(unittest.TestCase):
    def write(self, directory, name, changes=()):
        image = Image.new("RGBA", (10, 10), (100, 100, 100, 255))
        for (x, y), colour in changes:
            image.putpixel((x, y), colour)
        path = pathlib.Path(directory, name)
        image.save(path)
        return path

    def test_difference_counts_pixels_and_levels(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = self.write(tmp, "a.png")
            b = self.write(tmp, "b.png", [((0, 0), (101, 100, 100, 255)), ((5, 5), (100, 90, 100, 255))])
            difference = gpu.image_difference(a, b)
            self.assertEqual(difference["differing_pixels"], 2)
            self.assertEqual(difference["max_channel_delta"], 10)
            self.assertAlmostEqual(difference["differing_fraction"], 0.02)
            self.assertAlmostEqual(difference["mean_delta_of_differing"], 5.5)

    def test_identical_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = self.write(tmp, "a.png"), self.write(tmp, "b.png")
            self.assertEqual(gpu.image_difference(a, b)["differing_pixels"], 0)

    def test_tolerance_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = self.write(tmp, "a.png")
            b = self.write(tmp, "b.png", [((1, 1), (102, 100, 100, 255))])
            gpu.assert_images_close(self, a, b, max_channel_delta=2, max_differing_fraction=0.01)
            with self.assertRaises(AssertionError):
                gpu.assert_images_close(self, a, b, max_channel_delta=1, max_differing_fraction=0.01)
            with self.assertRaises(AssertionError):
                gpu.assert_images_close(self, a, b, max_channel_delta=2, max_differing_fraction=0.001)

    def test_sizes_must_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = self.write(tmp, "a.png")
            b = pathlib.Path(tmp, "b.png")
            Image.new("RGBA", (5, 5)).save(b)
            with self.assertRaises(ValueError):
                gpu.image_difference(a, b)


if __name__ == "__main__":
    unittest.main()
