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

from tools.shaders.fx_tokens import (FxError, Token, lex)

def _norm(path: str) -> str:
    value = posixpath.normpath(path.replace("\\", "/"))
    while value.startswith("./"):
        value = value[2:]
    return value


@dataclass
class ResolvedSource:
    text: str
    root: dict
    includes: list[dict]
    definitions: dict[str, str]


class IncludeResolver:
    """Case-insensitive, depth-first include resolver with cycle diagnostics."""

    def __init__(self, source_root: Path):
        self.root = source_root.resolve()
        if not self.root.is_dir():
            raise ValueError(f"source root is not a directory: {source_root}")
        self._index: dict[str, str] = {}
        for base, dirs, files in os.walk(self.root):
            dirs.sort(key=str.casefold)
            for filename in sorted(files, key=str.casefold):
                absolute = Path(base) / filename
                logical = _norm(absolute.relative_to(self.root).as_posix())
                key = logical.casefold()
                if key in self._index and self._index[key] != logical:
                    raise ValueError(f"case-colliding shader paths: {self._index[key]} and {logical}")
                self._index[key] = logical

    def _lookup(self, request: str, parent: str, line: int, column: int) -> str:
        candidates = []
        parent_dir = str(PurePosixPath(parent).parent)
        if parent_dir != ".":
            candidates.append(_norm(f"{parent_dir}/{request}"))
        candidates.append(_norm(request))
        for candidate in candidates:
            if candidate.startswith("../") or candidate == "..":
                continue
            found = self._index.get(candidate.casefold())
            if found is not None:
                return found
        raise FxError("SHD_INCLUDE_NOT_FOUND", f"include not found: {request}", parent, line, column)

    def resolve(self, effect: str) -> ResolvedSource:
        logical = self._index.get(_norm(effect).casefold())
        if logical is None:
            raise FxError("SHD_EFFECT_NOT_FOUND", f"effect not found: {effect}", _norm(effect), 1, 1)
        includes: list[dict] = []
        definitions: dict[str, str] = {}
        expanded: set[str] = set()

        def visit(current: str, stack: list[str]) -> str:
            key = current.casefold()
            if key in (item.casefold() for item in stack):
                cycle = " -> ".join([*stack, current])
                raise FxError("SHD_INCLUDE_CYCLE", f"include cycle: {cycle}", current, 1, 1)
            raw = (self.root / Path(current)).read_bytes()
            try:
                source = raw.decode("utf-8-sig")
            except UnicodeDecodeError as error:
                raise FxError("SHD_SOURCE_ENCODING", f"source is not UTF-8: {error}", current, 1, 1) from error
            tokens = lex(source, current)
            pieces: list[str] = [f'#line 1 "{current}"\n']
            cursor = 0
            active_stack: list[tuple[bool, bool, bool]] = []  # parent, branch taken, current
            active = True
            for token in tokens:
                if token.kind != "directive":
                    continue
                pieces.append(source[cursor:token.start] if active else "\n" * source[cursor:token.start].count("\n"))
                directive = token.value[1:].strip()
                command, _, argument = directive.partition(" ")
                command = command.lower()
                argument = argument.strip()
                replacement = ""
                if command == "include" and active:
                    match = re.fullmatch(r'["<]([^">]+)[">]', argument)
                    if not match:
                        raise FxError("SHD_INCLUDE_SYNTAX", "expected quoted include path", current, token.line, token.column)
                    child = self._lookup(match.group(1), current, token.line, token.column)
                    if child.casefold() in {item.casefold() for item in [*stack, current]}:
                        cycle = " -> ".join([*stack, current, child])
                        raise FxError("SHD_INCLUDE_CYCLE", f"include cycle: {cycle}", current, token.line, token.column)
                    if child.casefold() not in expanded:
                        expanded.add(child.casefold())
                        child_bytes = (self.root / Path(child)).read_bytes()
                        includes.append({"path": child, "sha256": hashlib.sha256(child_bytes).hexdigest()})
                        replacement = visit(child, [*stack, current])
                    replacement += f'\n#line {token.line + 1} "{current}"\n'
                elif command == "define" and active:
                    name_match = re.match(r"([A-Za-z_][A-Za-z0-9_]*)(.*)", argument, re.S)
                    if name_match:
                        name, value = name_match.group(1), name_match.group(2).strip()
                        if not value.startswith("("):
                            definitions[name] = value or "1"
                    replacement = token.value
                elif command in {"ifdef", "ifndef", "if"}:
                    parent = active
                    if command == "ifdef":
                        condition = argument in definitions
                    elif command == "ifndef":
                        condition = argument not in definitions
                    else:
                        condition = self._eval_condition(argument, definitions, current, token)
                    active = parent and condition
                    active_stack.append((parent, condition, active))
                    replacement = ""
                elif command == "elif":
                    if not active_stack:
                        raise FxError("SHD_PREPROCESSOR_NESTING", "#elif without #if", current, token.line, token.column)
                    parent, taken, _ = active_stack[-1]
                    condition = False if taken else self._eval_condition(argument, definitions, current, token)
                    active = parent and condition
                    active_stack[-1] = (parent, taken or condition, active)
                    replacement = ""
                elif command == "else":
                    if not active_stack:
                        raise FxError("SHD_PREPROCESSOR_NESTING", "#else without #if", current, token.line, token.column)
                    parent, taken, _ = active_stack[-1]
                    active = parent and not taken
                    active_stack[-1] = (parent, True, active)
                    replacement = ""
                elif command == "endif":
                    if not active_stack:
                        raise FxError("SHD_PREPROCESSOR_NESTING", "#endif without #if", current, token.line, token.column)
                    parent, _, _ = active_stack.pop()
                    active = parent
                    replacement = ""
                elif active:
                    replacement = token.value
                pieces.append(replacement)
                cursor = token.end
            pieces.append(source[cursor:] if active else "\n" * source[cursor:].count("\n"))
            if active_stack:
                raise FxError("SHD_PREPROCESSOR_NESTING", "unterminated conditional", current, tokens[-1].line, 1)
            return "".join(pieces)

        root_bytes = (self.root / Path(logical)).read_bytes()
        text = visit(logical, [])
        return ResolvedSource(
            text=text,
            root={"path": logical, "sha256": hashlib.sha256(root_bytes).hexdigest()},
            includes=includes,
            definitions=dict(sorted(definitions.items())),
        )

    @staticmethod
    def _eval_condition(expression: str, definitions: dict[str, str], path: str, token: Token) -> bool:
        # Directive tokens contain the complete physical line.  C/C++
        # preprocessors ignore a trailing comment, so remove it before applying
        # the deliberately small expression grammar.
        expression = re.sub(r"//.*$", "", expression).strip()
        value = re.sub(r"defined\s*\(\s*([A-Za-z_]\w*)\s*\)", lambda m: "1" if m.group(1) in definitions else "0", expression)
        value = re.sub(r"defined\s+([A-Za-z_]\w*)", lambda m: "1" if m.group(1) in definitions else "0", value)
        value = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: definitions.get(m.group(0), "0"), value)
        if not re.fullmatch(r"[\s0-9a-fA-FxX()!<>=&|+\-*/%]+", value):
            raise FxError("SHD_UNSUPPORTED_PREPROCESSOR", f"unsupported #if expression: {expression}", path, token.line, token.column)
        value = value.replace("&&", " and ").replace("||", " or ")
        value = re.sub(r"!(?!=)", " not ", value)
        try:
            return bool(eval(value, {"__builtins__": {}}, {}))  # controlled character set above
        except Exception as error:
            raise FxError("SHD_UNSUPPORTED_PREPROCESSOR", f"invalid #if expression: {expression}", path, token.line, token.column) from error
