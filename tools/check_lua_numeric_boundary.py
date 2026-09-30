#!/usr/bin/env python3
"""Compiler-backed check that the authoritative Lua path has no floating point.

Parses every translation unit of the authoritative Lua build with Clang's JSON
AST: the integer binary64 backend (src/script/numeric), the sflua glue and each
wrapper that compiles an upstream Lua 5.0.2 source (src/script/sflua). Every
AST node under namespace eawr::script whose source lies in the authoritative
files (including the upstream Lua sources and headers those units include) is
rejected if it has a floating type or is a floating literal. <math.h>/<cmath>
includes are rejected too. The benchmark-only hardware twin
(EAWR_SFLUA_HARDWARE_TWIN) is not part of this path.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import astuple, dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parent))
from boundary_cache import BoundaryCache  # noqa: E402

FLOATING_TYPE = re.compile(r"(?:^|[^A-Za-z_])(long double|double|float|__float128|_Float16)(?:[^A-Za-z_]|$)")
MATH_INCLUDE = re.compile(r"^\s*#\s*include\s*[<\"](?:math\.h|cmath|fenv\.h|cfenv)[>\"]", re.MULTILINE)
AUTHORED_ROOTS = ("include/eawr/script/numeric", "src/script/numeric", "src/script/sflua", "third_party/lua/src",
                  "third_party/lua/include")


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    column: int
    code: str
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}:{self.column}: {self.code}: {self.message}"


def display(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def translation_units(root: Path) -> list[Path]:
    units = sorted((root / "src/script/numeric").glob("*.cpp"))
    units += sorted((root / "src/script/sflua").glob("*.cpp"))
    units += sorted((root / "src/script/sflua/upstream").glob("*.cpp"))
    return [unit.resolve() for unit in units]


def authored_files(root: Path, extra: Iterable[Path]) -> set[str]:
    files = {os.path.normcase(str(path.resolve())) for path in extra}
    for relative in AUTHORED_ROOTS:
        base = root / relative
        if base.is_dir():
            files.update(os.path.normcase(str(path.resolve())) for path in base.rglob("*") if path.is_file())
    return files


def parse_json_stream(payload: str) -> Iterator[dict[str, Any]]:
    decoder = json.JSONDecoder()
    position = 0
    while position < len(payload):
        while position < len(payload) and payload[position].isspace():
            position += 1
        if position >= len(payload):
            return
        value, position = decoder.raw_decode(payload, position)
        if isinstance(value, dict):
            yield value


class LocationState:
    """Clang's JSON writes `file` and `line` only when they differ from the
    previously printed location, so both are tracked in document order."""

    def __init__(self, unit: Path):
        self.file = os.path.normcase(str(unit))
        self.line = 1

    def bare(self, location: dict[str, Any]) -> tuple[str, int, int]:
        if "file" in location:
            self.file = os.path.normcase(str(Path(location["file"]).resolve()))
        if "line" in location:
            self.line = int(location["line"])
        return self.file, self.line, int(location.get("col", 1))

    def visit(self, location: dict[str, Any]) -> tuple[str, int, int] | None:
        if not location:
            return None
        if "spellingLoc" in location or "expansionLoc" in location:
            spelled = self.bare(location.get("spellingLoc", {})) if "spellingLoc" in location else None
            if "expansionLoc" in location:
                self.bare(location["expansionLoc"])
            return spelled
        return self.bare(location)


def ast_violations(tree: dict[str, Any], authored: set[str], root: Path, unit: Path,
                   state: LocationState) -> set[Violation]:
    violations: set[Violation] = set()

    def walk(node: Any) -> None:
        if not isinstance(node, dict):
            return
        spelled = state.visit(node.get("loc", {}))
        node_range = node.get("range", {})
        begin = state.visit(node_range.get("begin", {}))
        state.visit(node_range.get("end", {}))
        current, line, column = spelled or begin or (state.file, state.line, 1)
        if current in authored:
            where = display(Path(current), root)
            kind = str(node.get("kind", ""))
            type_info = node.get("type")
            if isinstance(type_info, dict):
                for key in ("qualType", "desugaredQualType"):
                    text = type_info.get(key)
                    if isinstance(text, str) and FLOATING_TYPE.search(text):
                        violations.add(Violation(where, line, column, "LUA_NUMERIC_FLOATING_TYPE",
                                                 f"{kind or 'AST node'} has floating type '{text}'"))
                        break
            if kind == "FloatingLiteral":
                violations.add(Violation(where, line, column, "LUA_NUMERIC_FLOATING_LITERAL",
                                         "floating literal on the authoritative Lua path"))
        for child in node.get("inner", ()):
            walk(child)

    walk(tree)
    return violations


INCLUDE_DIRS = ("include", "src/script/sflua", "third_party/lua/include", "third_party/lua/src",
                "third_party/lua/src/lib")


def check_unit(unit: Path, root: Path, clang: str, authored: set[str],
               cache: BoundaryCache | None = None) -> tuple[Path, set[Violation], int]:
    key = {"unit": display(unit, root)}
    cached = cache.lookup(key) if cache is not None else None
    if cached is not None:
        return unit, {Violation(*item) for item in cached["violations"]}, cached["nodes"]
    with tempfile.TemporaryDirectory(prefix="eawr-lua-boundary-dep-") as scratch:
        depfile = Path(scratch) / "unit.d" if cache is not None else None
        result = run_unit(unit, root, clang, authored, depfile)
        _, violations, nodes = result
        if cache is not None and not any(v.code == "LUA_NUMERIC_COMPILE_ERROR" for v in violations):
            cache.store(key, depfile, {"violations": [list(astuple(v)) for v in sorted(violations)], "nodes": nodes})
    return result


def run_unit(unit: Path, root: Path, clang: str, authored: set[str],
             depfile: Path | None) -> tuple[Path, set[Violation], int]:
    command = [
        clang, "-std=c++20", "-fsyntax-only", "-fno-color-diagnostics", "-Wno-everything",
        "-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=eawr::script",
        "-I", str(root / "include"), "-I", str(root / "src/script/sflua"),
        "-I", str(root / "third_party/lua/include"), "-I", str(root / "third_party/lua/src"),
        "-I", str(root / "third_party/lua/src/lib"), str(unit),
    ]
    if depfile is not None:
        command += ["-MD", "-MF", str(depfile)]
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip() or f"exit {completed.returncode}")
        return unit, {Violation(display(unit, root), 1, 1, "LUA_NUMERIC_COMPILE_ERROR",
                                detail.replace("\n", " | ")[:2000])}, 0
    violations: set[Violation] = set()
    nodes = 0
    if not completed.stdout.strip():
        nodes = 0
    elif FLOATING_TYPE.search(completed.stdout) is None and "FloatingLiteral" not in completed.stdout:
        # No floating spelling anywhere in the dump: nothing to attribute, so the
        # (large) JSON is not parsed.
        nodes = completed.stdout.count('"kind": "NamespaceDecl"') or 1
    else:
        state = LocationState(unit)
        for tree in parse_json_stream(completed.stdout):
            nodes += 1
            violations |= ast_violations(tree, authored, root, unit, state)
    if nodes == 0:
        violations.add(Violation(display(unit, root), 1, 1, "LUA_NUMERIC_EMPTY_AST",
                                 "no declarations under eawr::script were parsed"))
    return unit, violations, nodes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--files", nargs="*", type=Path,
                        help="check these translation units (treated as authoritative) instead of the project set")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cache-dir", type=Path,
                        help="reuse the result of a unit whose every input is unchanged (tools/boundary_cache.py)")
    args = parser.parse_args()
    root = args.root.resolve()
    extra = [path if path.is_absolute() else root / path for path in (args.files or [])]
    units = [path.resolve() for path in extra] if args.files is not None else translation_units(root)
    authored = authored_files(root, extra)

    violations: set[Violation] = set()
    if not units:
        violations.add(Violation("<arguments>", 1, 1, "LUA_NUMERIC_EMPTY", "no translation units"))
    wrappers = {unit.stem for unit in units if unit.parent.name == "upstream"}
    if args.files is None and len(wrappers) < 23:
        violations.add(Violation("src/script/sflua/upstream", 1, 1, "LUA_NUMERIC_MISSING_UNIT",
                                 f"expected 23 upstream wrappers, found {len(wrappers)}"))
    for path in sorted(authored):
        if path.endswith((".c", ".h", ".cpp", ".hpp")) and "third_party" not in path.replace("\\", "/"):
            text = Path(path).read_text(encoding="utf-8", errors="replace")
            for match in MATH_INCLUDE.finditer(text):
                violations.add(Violation(display(Path(path), root), text.count("\n", 0, match.start()) + 1, 1,
                                         "LUA_NUMERIC_MATH_INCLUDE", "floating-point library include"))

    cache = None
    if args.cache_dir is not None:
        checker = Path(__file__).resolve()
        identity = {"checker": "lua_numeric_boundary", "root": os.path.normcase(str(root)),
                    "sources": [hashlib.sha256(source.read_bytes()).hexdigest()
                                for source in (checker, checker.with_name("boundary_cache.py"))],
                    # A unit's verdict depends on which files count as authored.
                    "authored": sorted(authored), **BoundaryCache.clang_identity(args.clang)}
        cache = BoundaryCache(args.cache_dir, root, identity, include_dirs=[root / d for d in INCLUDE_DIRS])

    print(f"Lua numeric boundary checked {len(units)} translation unit(s)")
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for unit, found, nodes in pool.map(lambda unit: check_unit(unit, root, args.clang, authored, cache), units):
            print(f"  {display(unit, root)}: {nodes} dumped declaration(s), {len(found)} violation(s)")
            violations |= found
    if cache is not None:
        print(f"Lua numeric boundary {cache.summary()}")
        cache.prune()

    if violations:
        print(f"Lua numeric boundary rejected {len(violations)} violation(s):", file=sys.stderr)
        for violation in sorted(violations):
            print(violation.render(), file=sys.stderr)
        return 1
    print("Lua numeric boundary accepted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
