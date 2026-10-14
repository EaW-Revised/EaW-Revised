"""Skip an installed-data CTest before starting its command when no root is available."""

import os
from pathlib import Path
import subprocess
import sys


def main(command):
    root = os.environ.get("EAWR_EAW_GAME_ROOT", "")
    if not root or not Path(root).is_dir():
        print("SKIPPED: installed game data requires an available EAWR_EAW_GAME_ROOT", flush=True)
        return 77
    # A supplied root still goes through the command's normal data validation and
    # assertions. In particular, corrupt/incomplete installed data must fail.
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
