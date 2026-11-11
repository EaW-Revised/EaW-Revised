#!/usr/bin/env python3
"""Package a completed Windows cross build on the host that owns its files."""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/common'))
import hidden_console
hidden_console.install()
sys.path.insert(0, str(ROOT / 'tools/release'))
import host_package
import kit_engine
import launcher_artifact


def unique(build: Path, name: str) -> Path:
    hits = [p for p in (build / 'apps').rglob(name) if p.is_file() and 'CMakeFiles' not in p.parts]
    if len(hits) != 1:
        raise ValueError(f'expected one {name} under {build / "apps"}, found {len(hits)}')
    return hits[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--engine-dir', required=True, type=Path)
    parser.add_argument('--launcher-artifact', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--symbol-store', required=True, type=Path)
    parser.add_argument('--revised-layer', type=Path)
    parser.add_argument('--allow-missing-revised-layer', action='store_true')
    parser.add_argument('--public', action='store_true')
    parser.add_argument('--debug', action='store_true', help='package the Debug build with same-commit release launcher inputs')
    parser.add_argument('--inputs-dir', type=Path, help='directory with the four same-commit Release launcher and manager binaries')
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r'[0-9a-f]{40}', args.sha):
            raise ValueError('--sha must be a full commit hash')
        head = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        if head != args.sha:
            raise ValueError(f'source checkout is at {head}, expected {args.sha}')
        launcher_artifact.verify_local(args.launcher_artifact, launcher_artifact.read_pin())
        if args.debug:
            if args.inputs_dir is None:
                raise ValueError('--debug requires --inputs-dir with same-commit Release binaries')
            launcher = args.inputs_dir / 'EAWR.exe'
            service = args.inputs_dir / 'eawr_mods_service.exe'
        else:
            if args.inputs_dir is not None:
                raise ValueError('--inputs-dir is only valid with --debug')
            launcher = unique(args.build_dir, 'EAWR.exe')
            service = unique(args.build_dir, 'eawr_mods_service.exe')
        with tempfile.TemporaryDirectory(prefix='eawr-kit-engine-') as scratch:
            kit_engine.stage_engine(Path(scratch), args.engine_dir)
            godot = Path(scratch) / 'godot' / 'Godot_v4.7.2-stable_win64.exe'
            command = host_package.packager_command(
                ROOT / 'tools/release/package_release.py', expect_identity=args.sha, build_dir=args.build_dir,
                version=args.version, out=args.out, symbol_store=args.symbol_store, godot=godot, launcher=launcher,
                mods_service=service, launcher_artifact=args.launcher_artifact, revised_layer=args.revised_layer,
                allow_missing_revised_layer=args.allow_missing_revised_layer, public=args.public, debug=args.debug)
            subprocess.run(command, check=True)
        for line in host_package.release_file_lines(args.out):
            print(line)
        return 0
    except (OSError, ValueError, kit_engine.KitEngineError, subprocess.SubprocessError) as error:
        print(f'package-cross: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
