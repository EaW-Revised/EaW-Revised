#!/usr/bin/env python3
"""Translate a bounded legacy Direct3D effect into validated SPIR-V stages.

Private inputs and every source-derived output belong under ignored ``out/shaders``.
Checked-in code contains only the independently written parser/lowering and synthetic
fixtures.  Run with ``--help`` for the reproducible corpus command.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Optional

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.shaders.fx_parser import (Effect, FxError, IncludeResolver, Pass,
                                         Technique, parameter_dict, parse_effect,
                                         prune_unreachable_functions,
                                         source_without_effect_wrappers)
else:
    from .fx_parser import (Effect, FxError, IncludeResolver, Pass, Technique,
                            parameter_dict, parse_effect,
                            prune_unreachable_functions,
                            source_without_effect_wrappers)


from tools.shaders.translate_toolchain import (
    TOOL_VERSION, ARCHIVE_SHA256, REPOSITORY_ROOT, _write_text_lf, ShaderPolicyError, canonical_json, sha256_file, _resolve_output_path, _platform_identity, _toolchain_contract, _validate_tool_identity, _find_tool
)
from tools.shaders.translate_state import (
    EFFECTS, COMPATIBILITY, _split_top_level_expressions, normalize_legacy_hlsl, is_editor_technique, _normal, select_compatible, evaluate_dynamic_bool, skin_transform, _state, _bool_state, render_state_descriptor, _enum, SUPPORTED_OPS, SUPPORTED_ARGS, fixed_function_ir, evaluate_fixed_function, _arg_expr, _op_expr, lower_fixed_function
)
from tools.shaders.translate_compile import (
    preprocess_effect, resolve_compiled_shader_symbols, compile_stage, _verify_contract, _archive_path, _technique_dict, translate
)

_write_text_lf.__module__ = __name__
ShaderPolicyError.__module__ = __name__
canonical_json.__module__ = __name__
sha256_file.__module__ = __name__
_split_top_level_expressions.__module__ = __name__
normalize_legacy_hlsl.__module__ = __name__
is_editor_technique.__module__ = __name__
_resolve_output_path.__module__ = __name__
_platform_identity.__module__ = __name__
_toolchain_contract.__module__ = __name__
_validate_tool_identity.__module__ = __name__
_normal.__module__ = __name__
select_compatible.__module__ = __name__
evaluate_dynamic_bool.__module__ = __name__
skin_transform.__module__ = __name__
_state.__module__ = __name__
_bool_state.__module__ = __name__
render_state_descriptor.__module__ = __name__
_enum.__module__ = __name__
fixed_function_ir.__module__ = __name__
evaluate_fixed_function.__module__ = __name__
_arg_expr.__module__ = __name__
_op_expr.__module__ = __name__
lower_fixed_function.__module__ = __name__
preprocess_effect.__module__ = __name__
resolve_compiled_shader_symbols.__module__ = __name__
compile_stage.__module__ = __name__
_verify_contract.__module__ = __name__
_archive_path.__module__ = __name__
_technique_dict.__module__ = __name__
translate.__module__ = __name__
_find_tool.__module__ = __name__


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True, help="explicit directory containing effect sources")
    parser.add_argument("--source-archive", type=Path, help="foc_shaders.zip; auto-discovered in a source-root ancestor")
    parser.add_argument("--effect", required=True, help="one bounded effect name/path")
    parser.add_argument("--out", type=Path, required=True, help="explicit local generated-artifact directory")
    parser.add_argument("--toolchain-root", type=Path, help="directory containing pinned bin tools")
    parser.add_argument("--glslang", type=Path, help="explicit pinned glslang executable")
    parser.add_argument("--spirv-val", type=Path, help="explicit pinned SPIRV-Tools validator")
    parser.add_argument("--allow-unpinned-synthetic", action="store_true", help="permit an original synthetic fixture outside the bounded corpus")
    parser.add_argument("--pinned-public-corpus", action="store_true",
                        help="permit any effect from the archive after verifying the public archive hash")
    args = parser.parse_args(argv)
    try:
        output = _resolve_output_path(args.out)
        glslang = _find_tool(args.glslang, args.toolchain_root, "bin/glslang.exe", "glslang", "glslang")
        spirv_val = _find_tool(args.spirv_val, args.toolchain_root, "bin/spirv-val.exe", "spirv-val", "spirv_tools")
        translate(args.source_root, args.effect, output, glslang, spirv_val,
                  args.source_archive, args.allow_unpinned_synthetic,
                  args.pinned_public_corpus)
    except FxError as error:
        print(str(error), file=sys.stderr)
        return 2
    except ShaderPolicyError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError) as error:
        print(f"SHD_TRANSLATION_FAILED: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
