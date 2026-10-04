"""Token/structure aware parser for the bounded Alamo ``.fx`` translation spike.

This is an independent implementation.  It deliberately keeps source ranges and
never finds blocks by regular-expression brace matching.  The lexer understands
comments, quoted strings, preprocessor lines and nested ``{}``, ``()``, ``[]`` and
``<>`` groups.  The parser only assigns meaning to the effect constructs required
by P0-10; unknown constructs are retained in the IR or diagnosed, never erased by
an unstructured text substitution.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
from typing import Iterable, Iterator, Optional


import sys
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.shaders.fx_tokens import (
    FxError, Token, _TWO_CHAR, lex, _matching
)
from tools.shaders.fx_include import (
    _norm, ResolvedSource, IncludeResolver
)
from tools.shaders.fx_ir import (
    Parameter, Sampler, ShaderAssignment, Pass, Technique, Effect
)
from tools.shaders.fx_declarations import (
    _slice, _match_angle, _find_top_level, _assignments, _shader_assignment, _annotation_map, parse_effect, _parse_struct_fields, _parse_declaration, _parse_passes, source_without_effect_wrappers, prune_unreachable_functions, parameter_dict
)

FxError.__module__ = __name__
Token.__module__ = __name__
lex.__module__ = __name__
_matching.__module__ = __name__
_norm.__module__ = __name__
ResolvedSource.__module__ = __name__
IncludeResolver.__module__ = __name__
Parameter.__module__ = __name__
Sampler.__module__ = __name__
ShaderAssignment.__module__ = __name__
Pass.__module__ = __name__
Technique.__module__ = __name__
Effect.__module__ = __name__
_slice.__module__ = __name__
_match_angle.__module__ = __name__
_find_top_level.__module__ = __name__
_assignments.__module__ = __name__
_shader_assignment.__module__ = __name__
_annotation_map.__module__ = __name__
parse_effect.__module__ = __name__
_parse_struct_fields.__module__ = __name__
_parse_declaration.__module__ = __name__
_parse_passes.__module__ = __name__
source_without_effect_wrappers.__module__ = __name__
prune_unreachable_functions.__module__ = __name__
parameter_dict.__module__ = __name__
