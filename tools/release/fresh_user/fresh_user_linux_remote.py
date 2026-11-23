#!/usr/bin/env python3
"""Run the Linux fresh-user gate on the clean Ubuntu 24.04 box from this workstation.

  python tools/release/fresh_user/fresh_user_linux_remote.py --package <linux player zip> --out <local dir>
                                                             --game-root <FoC copy on the box> [--expected-sha256 <hex>]

Environment (nothing host-specific is committed): EAWR_LINUX_FRESH_HOST, EAWR_LINUX_FRESH_USER (plain user, no sudo),
EAWR_LINUX_FRESH_IDENTITY (ssh key), optional EAWR_LINUX_FRESH_KNOWN_HOSTS (a known_hosts file for the box) and
EAWR_LINUX_FRESH_GAME (default for --game-root; one of the two is REQUIRED, exit 2 without it).
The box needs xvfb and mesa-vulkan-drivers as the gate's own tooling (not player dependencies; docs/releasing.md).

Copies the zip and the two gate files to the box, runs fresh_user_linux.py there, fetches fresh-user-verdict.json, the receipt
and the logs into --out, removes the staged zip and exits with the gate's status. A library the player needs that the stock
image lacks is reported in the failed case's detail: send the exact apt names to the maintainer (docs list what a player installs).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "tools" / "common"))
import hidden_console  # noqa: E402

hidden_console.install()


def need(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise SystemExit(f"fresh-user-linux-remote: set {name}")
    return value


def step(command: list[str], what: str, **kwargs) -> bool:
    """Runs an ssh/scp command; on failure prints a plain message (never the command, user, host or key) and returns False."""
    try:
        # ssh/scp's own error text names the host, so it is dropped too.
        done = subprocess.run(command, check=False, stderr=subprocess.DEVNULL, **kwargs)
    except OSError:
        print(f"fresh-user-linux-remote: {what} failed (the ssh/scp tool could not be started)", file=sys.stderr)
        return False
    if done.returncode != 0:
        print(f"fresh-user-linux-remote: {what} failed (exit {done.returncode}); check the box settings in the environment variables", file=sys.stderr)
        return False
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--game-root", default=os.environ.get("EAWR_LINUX_FRESH_GAME", ""),
                        help="REQUIRED (or EAWR_LINUX_FRESH_GAME): path ON THE BOX of a FoC copy")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--keep-staged", action="store_true")
    args = parser.parse_args(argv)
    if not args.game_root:
        print("fresh-user-linux-remote: --game-root (or EAWR_LINUX_FRESH_GAME) is required: the gate cannot pass without the game folder", file=sys.stderr)
        return 2
    target = f"{need('EAWR_LINUX_FRESH_USER')}@{need('EAWR_LINUX_FRESH_HOST')}"
    options = ["-i", str(Path(need("EAWR_LINUX_FRESH_IDENTITY")).expanduser()), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes"]
    if os.environ.get("EAWR_LINUX_FRESH_KNOWN_HOSTS"):
        options += ["-o", "UserKnownHostsFile=" + os.environ["EAWR_LINUX_FRESH_KNOWN_HOSTS"], "-o", "StrictHostKeyChecking=accept-new"]
    ssh = ["ssh", *options, "-o", "ConnectTimeout=15", target]
    scp = ["scp", *options]
    # The hash of THIS workstation's file is always handed to the gate: the box's copy must match it byte for byte.
    local_sha = hashlib.sha256(args.package.read_bytes()).hexdigest()
    if args.expected_sha256 and args.expected_sha256.lower() != local_sha:
        print(f"fresh-user-linux-remote: {args.package} has sha256 {local_sha}, not the expected {args.expected_sha256}", file=sys.stderr)
        return 2
    remote = f"fresh-linux/{time.strftime('%Y%m%d-%H%M%S')}"
    args.out.mkdir(parents=True, exist_ok=True)
    # A failing ssh/scp prints one plain line: no traceback, so no user, host, IP or key path reaches a log.
    if not (step(ssh + [f"mkdir -p {remote}/gate"], "preparing the staging folder on the box")
            # scp reads "C:\..." as host:path: run it from the file's folder with bare names.
            and step(scp + [args.package.name, f"{target}:{remote}/"], "copying the package to the box", cwd=args.package.resolve().parent)
            and step(scp + ["fresh_user_linux.py", "container_checks.sh", "press_escape.py", f"{target}:{remote}/gate/"],
                     "copying the gate files to the box", cwd=HERE)):
        return 2
    gate = ["python3", f"{remote}/gate/fresh_user_linux.py", "--package", f"{remote}/{args.package.name}", "--out", f"{remote}/out",
            "--machine", "clean-ubuntu-box", "--game-root", args.game_root]
    gate += ["--expected-sha256", local_sha]
    verdict = args.out / "fresh-user-verdict.json"
    verdict.unlink(missing_ok=True)  # only a verdict fetched by THIS run may be printed
    code = subprocess.run(ssh + [shlex.join(gate)]).returncode
    fetched = subprocess.run(ssh + [f"tar -C {remote}/out -c --exclude=home --exclude=home-empty ."], capture_output=True)
    if fetched.returncode == 0:
        subprocess.run(["tar", "-x", "-C", "."], input=fetched.stdout, check=False, cwd=args.out.resolve())
    if verdict.is_file():
        import json
        value = json.loads(verdict.read_text(encoding="utf-8"))
        print(f"fresh-user linux-x64: {value['result']} ({value['image']}; cases {value['cases_passed']} passed / {value['cases_failed']} failed)")
        for case in value["cases"]:
            print(f"  {case['status']:4} {case['name']}: {case['detail'][:300]}")
    else:
        print("fresh-user-linux-remote: no verdict came back (the gate itself failed on the box)", file=sys.stderr)
        code = code or 2
    if not args.keep_staged:
        subprocess.run(ssh + [f"chmod -R u+w {remote} 2>/dev/null; rm -rf {remote}"], check=False)
    return code


if __name__ == "__main__":
    sys.exit(main())
