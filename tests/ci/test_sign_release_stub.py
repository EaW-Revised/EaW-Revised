import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "release"))
import sign_release  # noqa: E402


class SignReleaseStubTest(unittest.TestCase):
    def test_off_by_default(self):
        self.assertEqual(sign_release.main([], {}), 0)
        self.assertEqual(sign_release.main([], {"EAWR_SIGNING": "0"}), 0)

    def test_enabled_refuses_until_implemented(self):
        self.assertEqual(sign_release.main([], {"EAWR_SIGNING": "1"}), 2)


if __name__ == "__main__":
    unittest.main()
