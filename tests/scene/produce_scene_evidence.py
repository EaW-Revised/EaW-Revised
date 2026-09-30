"""Materialize the original synthetic fixture and run the imported-scene producer."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", default=os.environ.get("EAWR_EVIDENCE_TARGET"))
    parser.add_argument("--build-id", default=os.environ.get("EAWR_EVIDENCE_BUILD_ID"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.target or not args.build_id:
        parser.error("--target and --build-id (or their EAWR_EVIDENCE_* environment variables) are required")
    spec = importlib.util.spec_from_file_location("eawr_scene_fixture", args.fixture)
    if spec is None or spec.loader is None:
        parser.error(f"cannot import fixture {args.fixture}")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    # Git may check out Python sources with CRLF on Windows. Bind evidence to
    # the repository's LF form so every platform records the same fixture ID.
    fixture_hash = hashlib.sha256(args.fixture.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    with tempfile.TemporaryDirectory(prefix="eawr-scene-evidence-") as temp:
        root = fixture.write_fixture_root(Path(temp))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".scene-evidence-stage-", dir=args.output.parent) as stage_name:
            stage = Path(stage_name)
            for workers in (1, 2, 4):
                lane = stage / f"workers-{workers}"
                command = [str(args.program), "--fixture-root", str(root), "--output", str(lane),
                           "--target", args.target, "--build-id", args.build_id,
                           "--fixture-sha256", fixture_hash, "--workers", str(workers)]
                if args.self_test:
                    command.append("--self-test")
                subprocess.run(command, check=True)
            # Only a complete set becomes visible at the artifact path.
            backup_dir = Path(tempfile.mkdtemp(prefix=".scene-evidence-backup-", dir=args.output.parent))
            backup = backup_dir / "previous-evidence"
            published = False
            try:
                if args.output.exists():
                    args.output.rename(backup)
                try:
                    stage.rename(args.output)
                except OSError:
                    if backup.exists():
                        backup.rename(args.output)
                    raise
                published = True
            finally:
                if published:
                    shutil.rmtree(backup_dir)
                elif not backup.exists():
                    backup_dir.rmdir()


if __name__ == "__main__":
    main()
