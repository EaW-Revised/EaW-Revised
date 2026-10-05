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


@dataclass
class Parameter:
    name: str
    type: str
    array_size: Optional[str] = None
    semantic: Optional[str] = None
    annotation: Optional[str] = None
    default: Optional[str] = None
    register: Optional[str] = None
    line: int = 1


@dataclass
class Sampler:
    name: str
    type: str
    texture: Optional[str]
    states: dict[str, str]
    register: Optional[str]
    line: int


@dataclass
class ShaderAssignment:
    profile: Optional[str]
    entry: Optional[str]
    expression: str


@dataclass
class Pass:
    name: str
    states: dict[str, str]
    vertex_shader: Optional[ShaderAssignment]
    pixel_shader: Optional[ShaderAssignment]
    line: int
    draw: bool = True


@dataclass
class Technique:
    name: str
    annotations: dict[str, str]
    passes: list[Pass]
    line: int


@dataclass
class Effect:
    parameters: list[Parameter] = field(default_factory=list)
    samplers: list[Sampler] = field(default_factory=list)
    techniques: list[Technique] = field(default_factory=list)
    structs: dict[str, list[Parameter]] = field(default_factory=dict)
    removable_ranges: list[tuple[int, int, str]] = field(default_factory=list)
    unsupported: list[dict] = field(default_factory=list)
