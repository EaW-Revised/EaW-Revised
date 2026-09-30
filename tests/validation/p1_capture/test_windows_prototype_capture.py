import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from windows_capture_test_support import *


if __name__ == "__main__":
    from test_windows_capture_process import SyntheticObservationTests, ArgvPolicyTests, LiveObserverTests
    from test_windows_capture_evidence import CaptureCliTests
    unittest.main()
