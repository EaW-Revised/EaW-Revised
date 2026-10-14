#!/usr/bin/env python3
"""Exercise every intended sim-boundary rejection and accepted counterexample."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


NEGATIVE = {
    "alias_float.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "auto_deduction.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "direct_os_io.cpp": "SIM_BOUNDARY_FORBIDDEN_INCLUDE",
    "direct_os_call.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "float_arithmetic.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "floating_literal.cpp": "SIM_BOUNDARY_FLOATING_LITERAL",
    "long_double.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "render_include.cpp": "SIM_BOUNDARY_FORBIDDEN_INCLUDE",
}
POSITIVE = ("comment_float.cpp", "fixed_integer.cpp", "renderer_float.cpp")


def invoke(checker: Path, root: Path, clang: str, fixture: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(checker), "--root", str(root), "--clang", clang, "--files", str(fixture)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    checker = root / "tools" / "check_sim_boundary.py"
    fixture_root = root / "tests" / "core" / "sim_boundary"
    failures = 0

    for name, expected in NEGATIVE.items():
        result = invoke(checker, root, args.clang, fixture_root / "negative" / name)
        if result.returncode == 0 or expected not in result.stdout:
            print(f"FAIL negative/{name}: expected rejection containing {expected}", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            failures += 1
        else:
            print(f"PASS negative/{name}: rejected with {expected}")

    for name in POSITIVE:
        result = invoke(checker, root, args.clang, fixture_root / "positive" / name)
        if result.returncode != 0:
            print(f"FAIL positive/{name}: expected acceptance", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            failures += 1
        else:
            print(f"PASS positive/{name}: accepted")

    # Exercise the actual runtime-loader path/namespace and its narrow converter exception.
    with tempfile.TemporaryDirectory(prefix="eawr-loader-boundary-") as temporary:
        sandbox = Path(temporary)
        (sandbox / "include").mkdir()
        (sandbox / "include/input.hpp").write_text(
            "#include <bit>\n#include <cstdint>\n"
            "namespace eawr::assets { struct Input { float value; }; Input mutate(float); }\n"
            "namespace eawr::scene { int fixed_from_binary32(float); }\n", encoding="utf-8")
        cases = (
            ("src/script/foc/new_host.cpp", "auto decision() { return 1.0; }", False),
            ("src/script/authoritative/new_host.cpp", "using Scalar = double; Scalar decision();", False),
            ("src/scene/scene.cpp", "int fixed_from_binary32(float value) { return static_cast<int>(std::bit_cast<std::uint32_t>(value)); }", True),
            ("src/scene/scene.cpp", "int fixed_from_binary32(float value) { return static_cast<int>(value * 2); }", False),
            ("src/units/unit_support.cpp", "auto load(assets::Input a) { return scene::fixed_from_binary32(a.value); }", True),
            ("src/skirmish/inputs.cpp", "auto load(assets::Input a) { return scene::fixed_from_binary32(a.value); }", True),
            ("src/units/unit_support.cpp", "auto load(assets::Input a) { return scene::fixed_from_binary32(assets::mutate(a.value).value); }", False),
            ("src/units/new_loader.cpp", "auto load(assets::Input a) { return scene::fixed_from_binary32(a.value); }", False),
            ("src/units/unit_support.cpp", "auto load(assets::Input a) { return scene::fixed_from_binary32(a.value + a.value); }", False),
            ("src/units/unit_support.cpp", "auto load() { return scene::fixed_from_binary32(1.0f); }", False),
            ("src/units/unit_support.cpp", "auto load() { return scene::fixed_from_binary32(1); }", False),
            ("src/units/unit_support.cpp", "auto load(assets::Input a) { auto copy = a.value; return scene::fixed_from_binary32(copy); }", False),
            ("src/skirmish/new_loader.cpp", "auto load() { return 1.0; }", False),
            ("src/data/new_loader.cpp", "using Scalar = double; Scalar load();", False),
            ("src/data/new_loader.cpp", "int load() { int close = 1; return close; }", True),
            ("src/script/foc/new_host.cpp", "int load() { int values[] = {1, 2}; return *std::remove(values, values + 2, 1); }", True),
        )
        for relative, body, accepted in cases:
            path = sandbox / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            namespace = relative.split("/")[1]
            path.write_text('#include "input.hpp"\n#include <algorithm>\nnamespace eawr::' + namespace + ' { ' + body + ' }\n', encoding="utf-8")
            result = invoke(checker, sandbox, args.clang, path)
            if (result.returncode == 0) != accepted or (not accepted and "SIM_BOUNDARY_FLOATING" not in result.stdout):
                print(f"FAIL loader {relative}: {body}\n{result.stdout}", file=sys.stderr)
                failures += 1

        # Aliases and function pointers cannot hide ambient-locale or decision-clock reads.
        for relative, body, expected in (
            ("src/units/new_loader.cpp", '#include <cctype>\nnamespace eawr::units { auto load(int c) { auto f = &std::toupper; return f(c); } }', "SIM_BOUNDARY_LOCALE"),
            ("src/script/foc/new_host.cpp", '#include <chrono>\nnamespace eawr::script { auto read() { using Clock = std::chrono::steady_clock; return Clock::now(); } }', "SIM_BOUNDARY_CLOCK"),
            ("src/sim/new_tick.cpp", '#include <ctime>\nnamespace eawr::sim { auto read() { auto f = &std::clock; return f(); } }', "SIM_BOUNDARY_CLOCK"),
            ("src/script/foc/new_host.cpp", 'extern "C" int remove(const char*);\nnamespace eawr::script { auto erase() { return remove("file"); } }', "SIM_BOUNDARY_DIRECT_IO"),
        ):
            path = sandbox / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body, encoding="utf-8")
            result = invoke(checker, sandbox, args.clang, path)
            if result.returncode == 0 or expected not in result.stdout:
                print(f"FAIL authoritative guard {relative}: {result.stdout}", file=sys.stderr)
                failures += 1

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
