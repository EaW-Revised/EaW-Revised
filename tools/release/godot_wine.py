#!/usr/bin/env python3
"""Run a Windows Godot through Wine, translating its project path."""
import os
from pathlib import Path
import sys

if os.name == 'nt':
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/common'))
    import hidden_console
    hidden_console.install()


def windows_path(value):
    path = Path(value).resolve()
    return 'Z:' + str(path).replace('/', '\\')


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        print('godot_wine: expected the Godot executable and arguments', file=sys.stderr)
        return 2
    args[0] = windows_path(args[0])
    for index, arg in enumerate(args[:-1]):
        if arg == '--path':
            args[index + 1] = windows_path(args[index + 1])
    wine = os.environ.get('EAWR_WINE', 'wine')
    # Replace the wrapper so a caller's timeout terminates Wine itself rather
    # than leaving its child process running after the wrapper is killed.
    try:
        os.execvpe(wine, [wine, *args], os.environ)
    except OSError as error:
        print(f'godot_wine: cannot run Wine ({wine}): {error}; set EAWR_WINE to a working Wine executable', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
