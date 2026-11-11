"""Disabled signing hook for the Windows release archives.

Signing is OFF by default: with no EAWR_SIGNING=1 this prints one line and exits 0, changes nothing and
touches no network. When switched on it refuses to run until the owner-controlled signing step is
implemented (design in docs/releasing.md, "Optional signing handoff"; prior draft: legacy #6018). It never
reads keys or credentials and must never run on a build worker.
"""
import os
import sys

ENABLE = "EAWR_SIGNING"


def main(argv=None, environ=None):
    environ = os.environ if environ is None else environ
    if environ.get(ENABLE) != "1":
        print("signing disabled: archives, SHA256SUMS and update-manifest stay unsigned (signature \"\")")
        return 0
    print("signing requested but not implemented: see docs/releasing.md 'Optional signing handoff'", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
