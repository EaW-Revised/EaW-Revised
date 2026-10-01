#!/bin/sh
set -eu
command -v python3 >/dev/null 2>&1 || { echo 'Install Python 3.8 or newer, then run this script again.' >&2; exit 2; }
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1 || { echo 'Install Python 3.8 or newer, then run this script again.' >&2; exit 2; }
exec python3 "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/demo.py" "$@"
