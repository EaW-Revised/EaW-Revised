"""Count the tests unittest will collect from a fresh-user harness."""
import importlib.util
import sys
import unittest
from pathlib import Path


def count_tests(path: Path) -> int:
    spec = importlib.util.spec_from_file_location("fresh_user_harness", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load harness: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    count = unittest.defaultTestLoader.loadTestsFromTestCase(module.PlayerFreshUser).countTestCases()
    if not count:
        raise ValueError("fresh-user harness has no tests")
    return count


if __name__ == "__main__":
    print(count_tests(Path(sys.argv[1]).resolve()))
