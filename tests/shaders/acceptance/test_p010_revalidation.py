#!/usr/bin/env python3
"""Targeted post-remediation checks for P0-10 policy and source diagnostics.

These checks use only authored synthetic files and metadata.  They deliberately
avoid displaying protected effect bodies, generated shader source, or compiler
logs.
"""

from __future__ import annotations

from pathlib import Path
import contextlib
import io
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

from tools.shaders.fx_parser import FxError, IncludeResolver
from tools.shaders.translate import (
    ShaderPolicyError,
    _resolve_output_path,
    main,
    translate,
)

FIXTURES = ROOT / "tests" / "shaders" / "fixtures"
TOOLCHAIN_ROOT = ROOT / "out" / "tools" / "glslang-16.5.0"
GLSLANG = TOOLCHAIN_ROOT / "bin" / "glslang.exe"
SPIRV_VAL = TOOLCHAIN_ROOT / "bin" / "spirv-val.exe"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


class P010PostRemediationChecks(unittest.TestCase):
    def test_nested_missing_include_preserves_actual_location(self) -> None:
        with tempfile.TemporaryDirectory(prefix="p010-location-") as temporary:
            root = Path(temporary)
            write(root / "Root.fx", '#include "nested/one.fxh"\n')
            write(root / "nested" / "one.fxh", "// one\n// two\n// three\n#include \"missing.fxh\"\n")
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("Root.fx")
            error = raised.exception
            self.assertEqual("SHD_INCLUDE_NOT_FOUND", error.code)
            self.assertEqual(("nested/one.fxh", 4, 1), (error.path, error.line, error.column))

    def test_traversal_and_symlink_escapes_leave_no_output(self) -> None:
        allowed = ROOT / "out" / "shaders"
        allowed.mkdir(parents=True, exist_ok=True)
        traversal = allowed / ".." / "p010-escape" / "translation"
        with self.assertRaises(ShaderPolicyError) as raised:
            _resolve_output_path(traversal)
        self.assertEqual("SHD_OUTPUT_OUTSIDE_CONFINEMENT", raised.exception.code)
        self.assertFalse((ROOT / "out" / "p010-escape").exists())

        with tempfile.TemporaryDirectory(dir=allowed, prefix="p010-link-") as parent_temp, \
             tempfile.TemporaryDirectory(prefix="p010-target-") as outside_temp:
            parent = Path(parent_temp)
            outside = Path(outside_temp)
            link = parent / "escape"
            try:
                link.symlink_to(outside, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks unavailable: {error}")
            escaped = link / "translation"
            with self.assertRaises(ShaderPolicyError) as raised:
                _resolve_output_path(escaped)
            self.assertEqual("SHD_OUTPUT_OUTSIDE_CONFINEMENT", raised.exception.code)
            self.assertFalse((outside / "translation").exists())

    def test_explicit_swapped_tools_fail_before_source_processing(self) -> None:
        self.assertTrue(GLSLANG.is_file() and SPIRV_VAL.is_file())
        with tempfile.TemporaryDirectory(dir=ROOT / "out" / "shaders", prefix="p010-preflight-") as output_parent:
            output = Path(output_parent) / "translation"
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = main([
                    "--source-root", str(ROOT / "does-not-exist"),
                    "--effect", "Synthetic.fx",
                    "--out", str(output),
                    "--glslang", str(SPIRV_VAL),
                    "--spirv-val", str(GLSLANG),
                    "--allow-unpinned-synthetic",
                ])
            self.assertEqual(2, result)
            self.assertIn("SHD_TOOL_IDENTITY_MISMATCH", stderr.getvalue())
            self.assertFalse(output.exists())

        with tempfile.TemporaryDirectory(dir=ROOT / "out" / "shaders", prefix="p010-api-preflight-") as output_parent:
            output = Path(output_parent) / "translation"
            with self.assertRaises(ShaderPolicyError) as raised:
                translate(
                    ROOT / "does-not-exist", "Synthetic.fx", output,
                    SPIRV_VAL, SPIRV_VAL, allow_unpinned_synthetic=True,
                )
            self.assertEqual("SHD_TOOL_IDENTITY_MISMATCH", raised.exception.code)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
