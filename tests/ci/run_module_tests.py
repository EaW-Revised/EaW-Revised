"""Runs `python -m unittest <module>` for CTest, or skips (exit 77, SKIP_RETURN_CODE) when a Python package the
module needs is not installed on this host.

    run_module_tests.py [--needs PIL numpy] <dotted.module>

The adapter tests import the P1-12 evidence code, which needs Pillow; the CTest hosts do not all carry it."""

import importlib.util
import subprocess
import sys

SKIP = 77


def main(argv: list[str]) -> int:
    needs: list[str] = []
    if argv and argv[0] == "--needs":
        argv = argv[1:]
        while len(argv) > 1 and not argv[0].startswith("tests."):
            needs.append(argv.pop(0))
    if len(argv) != 1:
        print(__doc__)
        return 2
    missing = [name for name in needs if importlib.util.find_spec(name) is None]
    if missing:
        print(f"SKIPPED {argv[0]}: not installed here: {', '.join(missing)}")
        return SKIP
    return subprocess.call([sys.executable, "-m", "unittest", argv[0]])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
