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


class FxError(Exception):
    """A source diagnostic with stable code and an original logical location."""

    def __init__(self, code: str, message: str, path: str, line: int, column: int):
        super().__init__(f"{path}:{line}:{column}: {code}: {message}")
        self.code = code
        self.message = message
        self.path = path
        self.line = line
        self.column = column

    def as_dict(self) -> dict:
        return {
            "code": self.code,
            "severity": "error",
            "message": self.message,
            "logical_path": self.path,
            "line": self.line,
            "column": self.column,
        }


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    start: int
    end: int
    line: int
    column: int
    path: str


_TWO_CHAR = {"==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "::", "<<", ">>"}


def lex(text: str, path: str = "<memory>") -> list[Token]:
    """Lex HLSL/effect text while preserving exact byte-independent source spans."""

    out: list[Token] = []
    i = 0
    line = 1
    column = 1
    line_has_code = False

    def advance(segment: str) -> None:
        nonlocal line, column, line_has_code
        newline_count = segment.count("\n")
        if newline_count:
            line += newline_count
            column = len(segment.rsplit("\n", 1)[1]) + 1
            line_has_code = bool(segment.rsplit("\n", 1)[1].strip())
        else:
            column += len(segment)

    while i < len(text):
        ch = text[i]
        if ch.isspace():
            start = i
            while i < len(text) and text[i].isspace():
                i += 1
            advance(text[start:i])
            continue

        start, start_line, start_column = i, line, column
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
            end = text.find("\n", i + 2)
            i = len(text) if end < 0 else end
            advance(text[start:i])
            continue
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            if end < 0:
                raise FxError("SHD_UNTERMINATED_COMMENT", "unterminated block comment", path, start_line, start_column)
            i = end + 2
            advance(text[start:i])
            continue
        if ch == "#" and not line_has_code:
            end = text.find("\n", i + 1)
            i = len(text) if end < 0 else end
            value = text[start:i]
            out.append(Token("directive", value, start, i, start_line, start_column, path))
            advance(value)
            line_has_code = True
            continue
        if ch in {'"', "'"}:
            quote = ch
            i += 1
            escaped = False
            while i < len(text):
                cur = text[i]
                if cur == "\n" and quote == '"':
                    raise FxError("SHD_UNTERMINATED_STRING", "newline in string literal", path, start_line, start_column)
                i += 1
                if escaped:
                    escaped = False
                elif cur == "\\":
                    escaped = True
                elif cur == quote:
                    break
            else:
                raise FxError("SHD_UNTERMINATED_STRING", "unterminated string literal", path, start_line, start_column)
            value = text[start:i]
            out.append(Token("string", value, start, i, start_line, start_column, path))
            advance(value)
            line_has_code = True
            continue
        if ch.isalpha() or ch == "_":
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] == "_"):
                i += 1
            value = text[start:i]
            out.append(Token("ident", value, start, i, start_line, start_column, path))
            advance(value)
            line_has_code = True
            continue
        if ch.isdigit() or (ch == "." and i + 1 < len(text) and text[i + 1].isdigit()):
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] in "._+-"):
                if text[i] in "+-" and text[i - 1] not in "eE":
                    break
                i += 1
            value = text[start:i]
            out.append(Token("number", value, start, i, start_line, start_column, path))
            advance(value)
            line_has_code = True
            continue
        two = text[i:i + 2]
        if two in _TWO_CHAR:
            i += 2
            out.append(Token("symbol", two, start, i, start_line, start_column, path))
            advance(two)
        else:
            i += 1
            out.append(Token("symbol", ch, start, i, start_line, start_column, path))
            advance(ch)
        line_has_code = True
    out.append(Token("eof", "", len(text), len(text), line, column, path))
    return out


def _matching(tokens: list[Token]) -> dict[int, int]:
    # Angle brackets are contextual in HLSL (annotations versus comparisons), so
    # they are matched only at grammar sites that expect annotations.
    pairs = {"{": "}", "(": ")", "[": "]"}
    closing = {v: k for k, v in pairs.items()}
    stacks: dict[str, list[int]] = {key: [] for key in pairs}
    result: dict[int, int] = {}
    for index, token in enumerate(tokens):
        if token.value in pairs:
            stacks[token.value].append(index)
        elif token.value in closing:
            opener = closing[token.value]
            if not stacks[opener]:
                raise FxError("SHD_UNBALANCED_BLOCK", f"unexpected '{token.value}'", token.path, token.line, token.column)
            begin = stacks[opener].pop()
            result[begin] = index
            result[index] = begin
    for opener, stack in stacks.items():
        if stack:
            token = tokens[stack[-1]]
            raise FxError("SHD_UNBALANCED_BLOCK", f"unterminated '{opener}' block", token.path, token.line, token.column)
    return result
