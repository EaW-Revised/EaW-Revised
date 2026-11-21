"""A small C++ tokenizer and the registration reader for the host binding tables.

Only what the binding reader needs: identifiers, string literals, punctuation, line numbers, matching
brackets and function-body ranges. Comments, character literals and preprocessor lines never produce tokens.
"""
from __future__ import annotations

import dataclasses
import re

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclasses.dataclass(frozen=True)
class Tok:
    kind: str  # id | str | p
    value: str
    line: int


def tokenize(text: str) -> list[Tok]:
    out: list[Tok] = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1; i += 1; continue
        if c in " \t\r\f\v":
            i += 1; continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j; continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j); i = j; continue
        if c == "#" and (not out or out[-1].line != line):
            # preprocessor line (with continuations)
            while i < n and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    line += 1; i += 2; continue
                i += 1
            continue
        if c == "R" and text.startswith('R"', i):
            m = re.compile(r'R"([^()\\ ]{0,16})\(').match(text, i)
            if m:
                end = text.find(")" + m.group(1) + '"', m.end())
                end = n if end < 0 else end
                out.append(Tok("str", text[m.end():end], line))
                line += text.count("\n", i, end); i = end + len(m.group(1)) + 2; continue
        if c == '"' or (c in "uUL" and text[i + 1:i + 2] == '"') or (text.startswith("u8\"", i)):
            if c != '"':
                i = text.index('"', i)
            j = i + 1; buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j:j + 2]); j += 2; continue
                if text[j] == "\n":
                    break
                buf.append(text[j]); j += 1
            out.append(Tok("str", "".join(buf), line)); i = j + 1; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1; continue
        m = _IDENT.match(text, i)
        if m:
            out.append(Tok("id", m.group(0), line)); i = m.end(); continue
        if c.isdigit():
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "._'"):
                j += 1
            i = j; continue
        if text.startswith("::", i) or text.startswith("->", i):
            out.append(Tok("p", text[i:i + 2], line)); i += 2; continue
        out.append(Tok("p", c, line)); i += 1
    return out


_OPEN = {"(": ")", "[": "]", "{": "}"}


def match_close(toks: list[Tok], index: int) -> int:
    """Index of the bracket that closes toks[index] (any of ( [ {); len(toks) when unbalanced."""
    stack = [_OPEN[toks[index].value]]
    for j in range(index + 1, len(toks)):
        v = toks[j].value
        if toks[j].kind != "p":
            continue
        if v in _OPEN:
            stack.append(_OPEN[v])
        elif stack and v == stack[-1]:
            stack.pop()
            if not stack:
                return j
    return len(toks)


def function_body(toks: list[Tok], name: str) -> tuple[int, int] | None:
    """Token range (open brace, close brace) of the first definition of the function `name`."""
    for i, t in enumerate(toks):
        if t.kind != "id" or t.value != name or i + 1 >= len(toks) or toks[i + 1].value != "(":
            continue
        if i and toks[i - 1].value in {".", "->"}:
            continue
        close = match_close(toks, i + 1)
        j = close + 1
        while j < len(toks) and toks[j].value not in {"{", ";", "="}:
            if toks[j].value == "(":
                j = match_close(toks, j)
            j += 1
        if j < len(toks) and toks[j].value == "{":
            return j, match_close(toks, j)
    return None


@dataclasses.dataclass(frozen=True)
class Registration:
    name: str
    kind: str  # global | member
    handle: str | None  # handle-kind identifier for members, when written literally
    path: str
    line: int
    via: str  # callee that registered it


def _string_arg_names(toks: list[Tok], start: int, stop: int) -> list[tuple[str, int]]:
    return [(t.value, t.line) for t in toks[start:stop] if t.kind == "str"]


def _first_arg_range(toks: list[Tok], open_paren: int) -> tuple[int, int]:
    """Token range [start, end) of the first call argument."""
    depth, j = 0, open_paren + 1
    start = j
    while j < len(toks):
        v = toks[j]
        if v.kind == "p" and v.value in _OPEN:
            j = match_close(toks, j) + 1; continue
        if v.kind == "p" and v.value in {",", ")"}:
            break
        j += 1
    return start, j


def _arg_ranges(toks: list[Tok], open_paren: int, limit: int = 3) -> list[tuple[int, int]]:
    close = match_close(toks, open_paren)
    out, j, start = [], open_paren + 1, open_paren + 1
    while j < close and len(out) < limit:
        v = toks[j]
        if v.kind == "p" and v.value in _OPEN:
            j = match_close(toks, j) + 1; continue
        if v.kind == "p" and v.value == ",":
            out.append((start, j)); start = j + 1
        j += 1
    if len(out) < limit and start <= close:
        out.append((start, close))
    return out


def _enclosing_loop_names(toks: list[Tok], index: int, lo: int) -> list[tuple[str, int]]:
    """String literals of the nearest `for (...)` header that encloses toks[index] (a loop over a name table)."""
    # A one-statement loop: `for (... : {"A", "B"}) add(name, f);` -- the call directly follows the header.
    if index - 1 >= lo and toks[index - 1].value == ")":
        d2, m = 0, index - 1
        while m >= lo:
            if toks[m].value == ")":
                d2 += 1
            elif toks[m].value == "(":
                d2 -= 1
                if d2 == 0:
                    break
            m -= 1
        if m > lo and toks[m - 1].value == "for":
            return _string_arg_names(toks, m, index - 1)
    depth = 0
    for j in range(index - 1, lo - 1, -1):
        t = toks[j]
        if t.kind == "p" and t.value == "}":
            depth += 1
        elif t.kind == "p" and t.value == "{":
            if depth:
                depth -= 1
                continue
            # the brace that opens the block we are in: is it the body of a for?
            k = j - 1
            if k >= 0 and toks[k].value == ")":
                # find matching open paren
                d2, m = 0, k
                while m >= lo:
                    if toks[m].value == ")":
                        d2 += 1
                    elif toks[m].value == "(":
                        d2 -= 1
                        if d2 == 0:
                            break
                    m -= 1
                if m > lo and toks[m - 1].value == "for":
                    return _string_arg_names(toks, m, k)
            # keep climbing out of non-loop blocks
    return []


API_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)?$")


def read_registrations(text: str, path: str, registrars: dict[str, str], function: str | None = None) -> list[Registration]:
    """Registrations in a source file: calls to a registrar whose name argument is a literal, or a name
    variable fed by an enclosing range-for over literals. `registrars` maps callee -> global|member.
    `function` restricts the reading to the body of one function."""
    toks = tokenize(text)
    lo, hi = 0, len(toks)
    if function:
        body = function_body(toks, function)
        if body is None:
            raise ValueError(f"{path}: function {function} not found")
        lo, hi = body
    found: list[Registration] = []
    seen: set[tuple[str, str, int]] = set()

    def add(name: str, kind: str, handle: str | None, line: int, via: str) -> None:
        if not API_NAME.match(name):
            return
        key = (name, kind, line)
        if key not in seen:
            seen.add(key)
            found.append(Registration(name, kind, handle, path, line, via))

    for i in range(lo, hi):
        t = toks[i]
        if t.kind != "id" or t.value not in registrars or i + 1 >= hi or toks[i + 1].value != "(":
            continue
        if i and toks[i - 1].value in {"void", "auto", "Result", ">"} and toks[i - 1].line == t.line:
            continue  # a declaration, not a call
        spec = registrars[t.value]  # global | member (name first) | member@2 (handle kind first, name second)
        kind = "global" if spec == "global" else "member"
        args = _arg_ranges(toks, i + 1, 3)
        if not args:
            continue
        handle_range = None
        if spec == "member@2" or (spec == "member" and t.value == "register_method"):
            if len(args) < 2:
                continue
            handle_range, name_range = args[0], args[1]
        else:
            name_range = args[0]
        handle = None
        if handle_range:
            ids = [x.value for x in toks[handle_range[0]:handle_range[1]] if x.kind == "id"]
            handle = ids[-1] if ids else None
        literals = _string_arg_names(toks, *name_range)
        if literals and len(literals) == 1 and name_range[1] - name_range[0] <= 6:
            add(literals[0][0], kind, handle, literals[0][1], t.value)
            continue
        # a name variable: the enclosing range-for header lists the names
        for name, line in _enclosing_loop_names(toks, i, lo):
            add(name, kind, handle, line, t.value + "(loop)")
    return found


def read_name_list(text: str, array_name: str, function: str | None = None) -> list[str]:
    """Quoted names in the braces that initialise the variable `array_name` (names only, no source)."""
    toks = tokenize(text)
    lo, hi = 0, len(toks)
    if function:
        body = function_body(toks, function)
        if body is None:
            raise ValueError(f"function {function} not found")
        lo, hi = body
    for i in range(lo, hi):
        t = toks[i]
        if t.kind == "id" and t.value == array_name:
            j = i + 1
            while j < hi and toks[j].value not in {"{", ";"}:
                j += 1
            if j < hi and toks[j].value == "{":
                return [x.value for x in toks[j:match_close(toks, j)] if x.kind == "str"]
    return []


def read_object_specs(text: str, function: str) -> list[str]:
    """Names from `ObjectSpec{"Name", fn, n, {{"Member", fn}, ...}}` rows in a function: `Name` and `Name.Member`."""
    toks = tokenize(text)
    body = function_body(toks, function)
    if body is None:
        raise ValueError(f"function {function} not found")
    out: list[str] = []
    for i in range(*body):
        if toks[i].kind == "id" and toks[i].value == "ObjectSpec" and i + 1 < len(toks) and toks[i + 1].value == "{":
            close = match_close(toks, i + 1)
            strings = [x for x in toks[i + 2:close] if x.kind == "str"]
            if not strings:
                continue
            name = strings[0].value
            out.append(name)
            out.extend(f"{name}.{x.value}" for x in strings[1:])
    return out


def called_identifiers(text: str, function: str | None = None) -> set[str]:
    """Identifiers directly followed by `(` in the file or in one function body (the callees a reader may follow)."""
    toks = tokenize(text)
    lo, hi = 0, len(toks)
    if function:
        body = function_body(toks, function)
        if body is None:
            raise ValueError(f"function {function} not found")
        lo, hi = body
    return {toks[i].value for i in range(lo, hi - 1) if toks[i].kind == "id" and toks[i + 1].value == "("}
