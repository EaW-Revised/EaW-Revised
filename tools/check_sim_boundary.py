#!/usr/bin/env python3
"""Compiler-backed numeric/dependency boundary for simulation and its setup loaders."""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import hashlib
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile
from functools import lru_cache
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parent))
from boundary_cache import BoundaryCache  # noqa: E402


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp"}
METADATA_NAMES = {"CMakeLists.txt", "sources.cmake", "README.md"}
FLOATING_TYPE = re.compile(r"(?:^|[^A-Za-z_])(long double|double|float|__float128)(?:[^A-Za-z_]|$)")
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE)
FORBIDDEN_INCLUDE = re.compile(
    r"(?:^|/)(?:filesystem|fstream|iostream|cstdio|stdio\.h|windows\.h|unistd\.h|"
    r"sys/|SDL\d*(?:/|\.h)|godot(?:/|_)|wgpu(?:/|\.h)|webgpu(?:/|\.h)|"
    r"vulkan(?:/|\.h)|GL(?:/|\.h)|d3d\w*\.h|eawr/(?:platform|presentation)/)",
    re.IGNORECASE,
)
DIRECT_IO_FUNCTIONS = {
    "CreateFileA",
    "CreateFileW",
    "DeleteFileA",
    "DeleteFileW",
    "ReadFile",
    "WriteFile",
    "close",
    "fclose",
    "fopen",
    "fread",
    "freopen",
    "fwrite",
    "open",
    "read",
    "remove",
    "write",
}


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    column: int
    code: str
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}:{self.column}: {self.code}: {self.message}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="repository root")
    parser.add_argument("--clang", required=True, help="clang++ executable used for AST parsing")
    parser.add_argument(
        "--files",
        nargs="*",
        type=Path,
        help="explicit files instead of discovering simulation, setup loaders and the binary32 decoder",
    )
    parser.add_argument("--jobs", type=int, default=1, help="Clang processes at once (default 1)")
    parser.add_argument("--cache-dir", type=Path,
                        help="reuse the result of a file whose every input is unchanged (tools/boundary_cache.py)")
    return parser.parse_args()


def relative_display(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def discover_project_files(root: Path) -> tuple[list[Path], list[Violation]]:
    files: list[Path] = []
    violations: list[Violation] = []
    for boundary_root in (root / prefix / component
                          for prefix in ("include/eawr", "src")
                          for component in ("sim", "units", "skirmish")):
        if not boundary_root.is_dir():
            violations.append(
                Violation(
                    relative_display(boundary_root, root),
                    1,
                    1,
                    "SIM_BOUNDARY_MISSING_ROOT",
                    "required simulation boundary root does not exist",
                )
            )
            continue
        for path in sorted(item for item in boundary_root.rglob("*") if item.is_file()):
            if path.suffix.lower() in SOURCE_SUFFIXES:
                files.append(path.resolve())
            elif path.name not in METADATA_NAMES:
                violations.append(
                    Violation(
                        relative_display(path, root),
                        1,
                        1,
                        "SIM_BOUNDARY_UNCHECKED_FILE",
                        "file under a simulation root has an unsupported extension",
                    )
                )
    files.append(root / "src/scene/scene.cpp")
    # Only the catalog/data paths called by setup; UI parsers are presentation data.
    for directory in (root / "include/eawr/data", root / "src/data"):
        if not directory.is_dir():
            violations.append(Violation(relative_display(directory, root), 1, 1,
                                        "SIM_BOUNDARY_MISSING_ROOT", "required setup-data root does not exist"))
            continue
        files.extend(path.resolve() for path in directory.iterdir()
                     if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
    if not files:
        violations.append(
            Violation(
                "include/eawr/sim,src/sim",
                1,
                1,
                "SIM_BOUNDARY_EMPTY",
                "no simulation headers or sources were discovered",
            )
        )
    return sorted(set(files)), violations


@lru_cache(maxsize=None)
def resolved_path(name: str) -> Path:
    return Path(name).resolve()


def source_location(node: dict[str, Any], inherited: Path | None, target: Path) -> tuple[Path, int, int]:
    loc = node.get("loc", {})
    begin = node.get("range", {}).get("begin", {})
    candidate = loc.get("spellingLoc", loc)
    begin_candidate = begin.get("spellingLoc", begin)
    file_name = candidate.get("file") or begin_candidate.get("file")
    current = resolved_path(file_name) if file_name else inherited or target
    line = int(candidate.get("line") or begin_candidate.get("line") or 1)
    column = int(candidate.get("col") or begin_candidate.get("col") or 1)
    return current, line, column


def type_spellings(node: dict[str, Any]) -> Iterable[str]:
    type_info = node.get("type")
    if not isinstance(type_info, dict):
        return ()
    return (
        spelling
        for key in ("qualType", "desugaredQualType")
        if isinstance((spelling := type_info.get(key)), str)
    )


def ast_violations(tree: dict[str, Any], target: Path, root: Path) -> list[Violation]:
    violations: set[Violation] = set()
    display = relative_display(target, root)
    target_resolved = target.resolve()
    source = target.read_bytes()

    # The sole numeric intake exception: asset float fields may be read directly
    # into the integer binary32 decoder. No float locals, literals or arithmetic.
    converter_files = {"src/units/unit_support.cpp", "src/skirmish/inputs.cpp"}
    read_kinds = {"ImplicitCastExpr", "DeclRefExpr", "MemberExpr", "ArraySubscriptExpr",
                  "CXXOperatorCallExpr", "ParenExpr"}

    def permitted_read(node: dict[str, Any]) -> bool:
        kind = node.get("kind")
        if kind not in read_kinds:
            return False
        if kind == "ImplicitCastExpr":
            return node.get("castKind") in {"LValueToRValue", "FunctionToPointerDecay", "NoOp"}
        if kind == "CXXOperatorCallExpr":
            callee = node.get("inner", [{}])[0]
            while callee.get("kind") == "ImplicitCastExpr" and callee.get("inner"):
                callee = callee["inner"][0]
            return callee.get("referencedDecl", {}).get("name") == "operator[]"
        return True

    def converter_call(node: dict[str, Any]) -> bool:
        if (display not in converter_files and display != "src/scene/scene.cpp") or node.get("kind") != "CallExpr":
            return False
        children = node.get("inner", [])
        if len(children) != 2:
            return False
        callee = children[0]
        while callee.get("kind") == "ImplicitCastExpr" and len(callee.get("inner", [])) == 1:
            callee = callee["inner"][0]
        span = callee.get("range", {})
        begin, end = span.get("begin", {}), span.get("end", {})
        spelling = source[begin.get("offset", 0):end.get("offset", 0) + end.get("tokLen", 0)]
        declaration = callee.get("referencedDecl", {})
        if display == "src/scene/scene.cpp":
            return (spelling == b"std::bit_cast<std::uint32_t>"
                    and declaration.get("name") == "bit_cast")
        if spelling not in (b"scene::fixed_from_binary32", b"eawr::scene::fixed_from_binary32"):
            return False
        return (callee.get("kind") == "DeclRefExpr"
                and declaration.get("name") == "fixed_from_binary32"
                and declaration.get("type", {}).get("qualType", "").endswith("(float)"))

    def walk(node: Any, inherited_file: Path | None = None, binary32_read: bool = False) -> None:
        if not isinstance(node, dict):
            return
        current_file, line, column = source_location(node, inherited_file, target)
        authored_here = current_file == target_resolved
        kind = str(node.get("kind", ""))

        if authored_here:
            spellings = tuple(type_spellings(node))
            floating = next((item for item in spellings if FLOATING_TYPE.search(item)), None)
            # The decoder's only floating declaration is its binary32 input.
            converter_declaration = display == "src/scene/scene.cpp" and (
                (kind == "FunctionDecl" and node.get("name") == "fixed_from_binary32")
                or (kind == "ParmVarDecl" and node.get("name") == "value"))
            if floating is not None and not (converter_declaration or (binary32_read and permitted_read(node))):
                violations.add(
                    Violation(
                        display,
                        line,
                        column,
                        "SIM_BOUNDARY_FLOATING_TYPE",
                        f"{kind or 'AST node'} has forbidden floating type '{floating}'",
                    )
                )
            if kind == "FloatingLiteral":
                violations.add(
                    Violation(
                        display,
                        line,
                        column,
                        "SIM_BOUNDARY_FLOATING_LITERAL",
                        "floating literal is forbidden in simulation code",
                    )
                )
            referenced = node.get("referencedDecl", {})
            referenced_name = referenced.get("name") if isinstance(referenced, dict) else None
            if (kind == "DeclRefExpr" and referenced_name in DIRECT_IO_FUNCTIONS
                    and referenced.get("kind") == "FunctionDecl"):
                violations.add(
                    Violation(
                        display,
                        line,
                        column,
                        "SIM_BOUNDARY_DIRECT_IO",
                        f"direct OS I/O function '{referenced_name}' is forbidden",
                    )
                )

        for child in node.get("inner", ()):
            walk(child, current_file, converter_call(node) or (binary32_read and permitted_read(node)))

    walk(tree)
    return sorted(violations)


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


def include_violations(path: Path, root: Path) -> list[Violation]:
    text = path.read_text(encoding="utf-8")
    display = relative_display(path, root)
    violations: list[Violation] = []
    for match in INCLUDE_DIRECTIVE.finditer(text):
        included = match.group(1)
        normalized = posixpath.normpath(included.replace("\\", "/"))
        if FORBIDDEN_INCLUDE.search(normalized):
            line = text.count("\n", 0, match.start()) + 1
            violations.append(
                Violation(
                    display,
                    line,
                    1,
                    "SIM_BOUNDARY_FORBIDDEN_INCLUDE",
                    f"render, engine, platform, or OS I/O include '{included}' is forbidden",
                )
            )
    return violations


def compile_ast(path: Path, root: Path, clang: str, generated: Path | None = None,
                depfile: Path | None = None) -> tuple[list[dict[str, Any]], str | None]:
    display = relative_display(path, root)
    namespace = "sim"
    for component in ("units", "skirmish", "data"):
        if display.startswith((f"src/{component}/", f"include/eawr/{component}/")):
            namespace = component
    if display == "src/scene/scene.cpp":
        # Scene also contains presentation float math; only this setup decoder
        # belongs to the authoritative numeric boundary.
        namespace = "scene::fixed_from_binary32"
    command = [
        clang,
        "-std=c++20",
        "-fsyntax-only",
        "-fno-color-diagnostics",
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        f"-ast-dump-filter=eawr::{namespace}",
        "-I",
        str(root / "include"),
        str(path),
    ]
    command.extend(["-I", str(root / "third_party/pugixml/src")])
    if generated is not None:
        command.extend(["-I", str(generated)])
    if depfile is not None:
        command.extend(["-MD", "-MF", str(depfile)])
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or f"compiler exited {completed.returncode}"
        return [], detail
    try:
        return list(parse_json_stream(completed.stdout)), None
    except json.JSONDecodeError as error:
        return [], f"could not parse Clang AST JSON: {error}"


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    if args.files is None:
        files, violations = discover_project_files(root)
    else:
        files = sorted({(path if path.is_absolute() else root / path).resolve() for path in args.files})
        violations = []
        if not files:
            violations.append(Violation("<arguments>", 1, 1, "SIM_BOUNDARY_EMPTY", "no files were provided"))

    print(f"simulation boundary checked file set ({len(files)} file(s)):")
    for path in files:
        print(f"  {relative_display(path, root)}")

    # A standalone checker must also work before CMake has generated the XML schema.
    with tempfile.TemporaryDirectory(prefix="eawr-boundary-") as temporary:
        generated = Path(temporary)
        # The immutable roster policy uses the same build-independent contract
        # as the XML schema: materialize it for this standalone compiler check.
        roster_generator = root / "tools/inventory/roster_gate.py"
        if roster_generator.is_file():
            subprocess.run([sys.executable, str(roster_generator), "--header",
                            str(generated / "roster_gate_data.hpp")], check=True)
        if any(path.name == "xml_registry.cpp" for path in files):
            subprocess.run([sys.executable, str(root / "src/data/generate_schema_contract.py"),
                            "--input", str(root / "plan/inventories/xml-tags.json"),
                            "--output", str(generated / "xml_schema_contract.inc")], check=True)
        cache = None
        if args.cache_dir is not None:
            checker = Path(__file__).resolve()
            identity = {"checker": "sim_boundary", "root": os.path.normcase(str(root)),
                        "sources": [hashlib.sha256(source.read_bytes()).hexdigest()
                                    for source in (checker, checker.with_name("boundary_cache.py"))],
                        **BoundaryCache.clang_identity(args.clang)}
            cache = BoundaryCache(args.cache_dir, root, identity,
                                  include_dirs=(root / "include", root / "third_party/pugixml/src", generated),
                                  aliases={generated: "<generated>"})
        return check_files(files, violations, root, args.clang, generated, max(1, args.jobs), cache)


def check_file(path: Path, root: Path, clang: str, generated: Path,
               cache: BoundaryCache | None) -> list[Violation]:
    if not path.is_file():
        return [Violation(relative_display(path, root), 1, 1, "SIM_BOUNDARY_MISSING_FILE", "file does not exist")]
    found = include_violations(path, root)
    key = {"file": relative_display(path, root)}
    cached = cache.lookup(key) if cache is not None else None
    if cached is not None:
        return found + [Violation(*item) for item in cached]
    with tempfile.TemporaryDirectory(prefix="eawr-boundary-dep-") as scratch:
        depfile = Path(scratch) / "unit.d"
        trees, compiler_error = compile_ast(path, root, clang, generated, depfile if cache is not None else None)
        if compiler_error is not None:
            return found + [Violation(relative_display(path, root), 1, 1, "SIM_BOUNDARY_COMPILE_ERROR",
                                      compiler_error.replace("\n", " | "))]
        ast_found = sorted({violation for tree in trees for violation in ast_violations(tree, path, root)})
        if cache is not None:
            cache.store(key, depfile, [list(dataclasses.astuple(violation)) for violation in ast_found])
    return found + ast_found


def check_files(files: list[Path], violations: list[Violation], root: Path,
                clang: str, generated: Path, jobs: int = 1, cache: BoundaryCache | None = None) -> int:
    # The XML registry (with the generated schema table) takes by far the longest: start it first.
    ordered = sorted(files, key=lambda path: path.name != "xml_registry.cpp")
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for found in pool.map(lambda path: check_file(path, root, clang, generated, cache), ordered):
            violations.extend(found)
    if cache is not None:
        print(f"simulation boundary {cache.summary()}")
        cache.prune()

    unique = sorted(set(violations))
    if unique:
        print(f"simulation boundary rejected {len(unique)} violation(s):", file=sys.stderr)
        for violation in unique:
            print(violation.render(), file=sys.stderr)
        return 1

    print("simulation boundary accepted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
