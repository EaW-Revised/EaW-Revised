"""Stage exactly the relay Dockerfile's build-stage COPY inputs and run its cmake steps there.

`docker build -f apps/relay_server/Dockerfile .` fails when a header the relay server includes is not COPYed. This test
reproduces that without a Docker daemon: it copies the Dockerfile's build-stage COPY sources from the repository into an
empty directory and runs the same `cmake` commands as the build stage's RUN step, with /build pointed at a temporary
directory. A missing input fails the configure or the compile, as it does in the image.

Usage: relay_dockerfile_stage.py [repo_root]
Exit 77 (ctest SKIP_RETURN_CODE) when the host is not Linux or has no cmake, ninja or C++ compiler.
"""
from __future__ import annotations

import fnmatch
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
CONTEXT_ROOT = Path(__file__).resolve().parents[2]
DOCKERFILE = Path("apps/relay_server/Dockerfile")


def logical_lines(text: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in text.splitlines():
        stripped = raw.strip()
        if not pending and (not stripped or stripped.startswith("#")):
            continue
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
            continue
        lines.append((pending + stripped).strip())
        pending = ""
    if pending:
        lines.append(pending.strip())
    return lines


def first_build_stage(dockerfile: Path) -> tuple[list[tuple[str, str]], str, list[str]]:
    """Return the first stage's COPY (source, destination) pairs, its WORKDIR and its RUN lines."""
    copies: list[tuple[str, str]] = []
    runs: list[str] = []
    workdir = "/"
    stages = 0
    for line in logical_lines(dockerfile.read_text(encoding="utf-8")):
        word, _, rest = line.partition(" ")
        word = word.upper()
        if word == "FROM":
            stages += 1
            if stages > 1:
                break
        elif word == "WORKDIR":
            workdir = rest.strip()
        elif word == "COPY":
            parts = shlex.split(rest)
            if any(part.startswith("--") for part in parts):
                raise AssertionError(f"build-stage COPY with flags is not supported by this test: {line}")
            if len(parts) != 2:
                raise AssertionError(f"build-stage COPY must be '<source> <destination>': {line}")
            copies.append((parts[0], parts[1]))
        elif word == "RUN":
            runs.append(rest)
    assert stages >= 1, "Dockerfile has no FROM"
    return copies, workdir, runs


def ignored(relative: Path, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(part, pattern) for part in relative.parts for pattern in patterns)


def stage_inputs(repo: Path, stage: Path, copies: list[tuple[str, str]], workdir: str, patterns: list[str]) -> None:
    for source, destination in copies:
        src = repo / source
        assert src.exists(), f"COPY source {source} does not exist in the repository"
        if destination.startswith("/"):
            raise AssertionError(f"absolute COPY destination {destination} is not supported by this test")
        dst = stage / workdir.lstrip("/") / destination
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True,
                            ignore=lambda directory, names: [
                                name for name in names if ignored((Path(directory) / name).relative_to(repo), patterns)])
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)


def cmake_commands(run: str, build_dir: Path) -> list[list[str]]:
    commands: list[list[str]] = []
    for part in run.split("&&"):
        argv = shlex.split(part)
        if argv and argv[0] == "cmake":
            commands.append([str(build_dir) if arg == "/build" else arg for arg in argv])
    return commands


def main() -> int:
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else CONTEXT_ROOT
    if platform.system() != "Linux":
        print("SKIP: the relay image is built on Linux")
        return SKIP
    for tool in ("cmake", "ninja"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found")
            return SKIP
    if not any(shutil.which(c) for c in (os.environ.get("CXX") or "", "g++", "c++", "clang++") if c):
        print("SKIP: no C++ compiler found")
        return SKIP
    copies, workdir, runs = first_build_stage(repo / DOCKERFILE)
    ignore_file = repo / (str(DOCKERFILE) + ".dockerignore")
    patterns = [l.strip() for l in ignore_file.read_text().splitlines() if l.strip() and not l.startswith("#")] \
        if ignore_file.exists() else []
    assert copies, "build stage has no COPY lines"
    with tempfile.TemporaryDirectory(prefix="eawr-relay-stage-") as root:
        stage = Path(root) / "context"
        build_dir = Path(root) / "build"
        stage.mkdir()
        stage_inputs(repo, stage, copies, workdir, patterns)
        work = stage / workdir.lstrip("/")
        commands = [c for run in runs for c in cmake_commands(run, build_dir)]
        assert commands, "build stage has no cmake RUN step"
        print("staged:", ", ".join(source for source, _ in copies), flush=True)
        for argv in commands:
            print("+", " ".join(argv), flush=True)
            result = subprocess.run(argv, cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            sys.stdout.write(result.stdout[-20000:])
            if result.returncode != 0:
                if "Could NOT find OpenSSL" in result.stdout:
                    print("SKIP: OpenSSL development files not found")
                    return SKIP
                print(f"FAIL: '{argv[0]} ... ' exited {result.returncode}; the Dockerfile COPY list is missing an "
                      "input of the relay server build")
                return 1
    print("relay Dockerfile staged build passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
