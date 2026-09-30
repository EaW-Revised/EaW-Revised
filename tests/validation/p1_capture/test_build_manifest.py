import sys
from pathlib import Path as _Path
sys.path.insert(0, str(_Path(__file__).resolve().parent))

from build_manifest_test_support import *


if __name__ == "__main__":
    from test_build_manifest_matrix import SuccessTests, ProductionDriftTests, PrototypeDriftTests, SupplementTests
    from test_build_manifest_artifacts import StrictJsonTests, EvidenceTests, PathPolicyTests
    unittest.main()
