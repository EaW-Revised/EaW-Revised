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


def _slice(text: str, tokens: list[Token], first: int, last_exclusive: int) -> str:
    if first >= last_exclusive:
        return ""
    return text[tokens[first].start:tokens[last_exclusive - 1].end]


def _match_angle(tokens: list[Token], start: int, end: Optional[int] = None) -> int:
    limit = len(tokens) if end is None else end
    depth = 0
    for index in range(start, limit):
        if tokens[index].value == "<":
            depth += 1
        elif tokens[index].value == ">":
            depth -= 1
            if depth == 0:
                return index
    token = tokens[start]
    raise FxError("SHD_UNBALANCED_ANNOTATION", "unterminated '<' annotation", token.path, token.line, token.column)


def _find_top_level(tokens: list[Token], start: int, end: int, value: str) -> Optional[int]:
    depth = {"(": 0, "[": 0, "{": 0}
    opens = set(depth)
    closes = {")": "(", "]": "[", "}": "{"}
    for index in range(start, end):
        current = tokens[index].value
        if current in opens:
            depth[current] += 1
        elif current in closes:
            depth[closes[current]] -= 1
        elif current == value and all(count == 0 for count in depth.values()):
            return index
    return None


def _assignments(text: str, tokens: list[Token], start: int, end: int) -> dict[str, str]:
    result: dict[str, str] = {}
    cursor = start
    while cursor < end:
        semi = _find_top_level(tokens, cursor, end, ";")
        if semi is None:
            break
        equal = _find_top_level(tokens, cursor, semi, "=")
        if equal is not None:
            key = "".join(t.value for t in tokens[cursor:equal]).strip()
            value = _slice(text, tokens, equal + 1, semi).strip()
            if key:
                result[key] = value
        cursor = semi + 1
    return result


def _shader_assignment(value: Optional[str]) -> Optional[ShaderAssignment]:
    if value is None or value.strip().casefold() == "null":
        return None
    parts = lex(value, "<shader-assignment>")[:-1]
    if parts and parts[0].value.casefold() == "compile":
        if len(parts) < 3 or parts[1].kind != "ident" or parts[2].kind != "ident":
            raise FxError("SHD_COMPILE_SYNTAX", f"invalid compile assignment: {value}", "<shader-assignment>", 1, 1)
        return ShaderAssignment(parts[1].value, parts[2].value, value.strip())
    for token in parts:
        if token.kind == "ident":
            return ShaderAssignment(None, token.value, value.strip())
    raise FxError("SHD_COMPILE_SYNTAX", f"unsupported shader assignment: {value}", "<shader-assignment>", 1, 1)


def _annotation_map(raw: str) -> dict[str, str]:
    tokens = lex(raw, "<annotation>")
    values = _assignments(raw, tokens, 0, len(tokens) - 1)
    return {key: value.strip().strip('"') for key, value in values.items()}


def parse_effect(text: str, path: str = "<flattened>") -> Effect:
    tokens = [token for token in lex(text, path) if token.kind not in {"directive", "eof"}]
    matching = _matching(tokens)
    effect = Effect()
    i = 0
    statement_start = 0
    qualifiers = {"uniform", "extern", "static", "const", "volatile", "row_major", "column_major", "shared"}

    while i < len(tokens):
        token = tokens[i]
        lower = token.value.casefold()
        if lower in {"technique", "technique10", "technique11"}:
            if i + 1 >= len(tokens) or tokens[i + 1].kind != "ident":
                raise FxError("SHD_TECHNIQUE_SYNTAX", "technique requires a name", token.path, token.line, token.column)
            name = tokens[i + 1].value
            cursor = i + 2
            annotations: dict[str, str] = {}
            if cursor < len(tokens) and tokens[cursor].value == "<":
                close = _match_angle(tokens, cursor)
                annotations = _annotation_map(_slice(text, tokens, cursor + 1, close))
                cursor = close + 1
            if cursor >= len(tokens) or tokens[cursor].value != "{":
                raise FxError("SHD_TECHNIQUE_SYNTAX", "technique requires a body", token.path, token.line, token.column)
            close = matching[cursor]
            passes = _parse_passes(text, tokens, matching, cursor + 1, close)
            effect.techniques.append(Technique(name, annotations, passes, token.line))
            effect.removable_ranges.append((token.start, tokens[close].end, "technique"))
            i = close + 1
            statement_start = i
            continue
        if lower in {"stateblock", "stateblock_state"}:
            cursor = i + 1
            if cursor < len(tokens) and tokens[cursor].kind == "ident":
                cursor += 1
            if cursor < len(tokens) and tokens[cursor].value == "=":
                cursor += 1
                if cursor < len(tokens) and tokens[cursor].value.casefold() == "stateblock_state":
                    cursor += 1
            if cursor < len(tokens) and tokens[cursor].value == "{":
                close = matching[cursor]
                end = close + 1
                if end < len(tokens) and tokens[end].value == ";":
                    end += 1
                effect.removable_ranges.append((token.start, tokens[end - 1].end, "stateblock"))
                i = end
                statement_start = i
                continue
        if lower == "struct" and i + 2 < len(tokens) and tokens[i + 1].kind == "ident" and tokens[i + 2].value == "{":
            close = matching[i + 2]
            effect.structs[tokens[i + 1].value] = _parse_struct_fields(text, tokens, i + 3, close)
            i = close
            if i + 1 < len(tokens) and tokens[i + 1].value == ";":
                i += 1
            statement_start = i + 1
            continue
        if token.value == ";":
            segment = tokens[statement_start:i]
            if segment:
                parsed = _parse_declaration(text, segment, matching, qualifiers)
                if parsed is not None:
                    parameter, sampler, ranges = parsed
                    if parameter is not None:
                        effect.parameters.append(parameter)
                    if sampler is not None:
                        effect.samplers.append(sampler)
                    effect.removable_ranges.extend(ranges)
            statement_start = i + 1
        elif token.value == "{" and i in matching:
            close = matching[i]
            # Function bodies are executable declarations, not global statements.
            # Initializer and sampler_state bodies remain part of their declaration;
            # jumping over them prevents their inner semicolons becoming globals.
            is_function = i > statement_start and tokens[i - 1].value in {":", ")"}
            if is_function:
                i = close
                if i + 1 < len(tokens) and tokens[i + 1].value == ";":
                    i += 1
                statement_start = i + 1
            else:
                i = close
        elif token.value == "<" and i > statement_start:
            # At global declaration scope an angle block following the declared
            # name is an effect annotation. Comparisons occur inside function
            # bodies, which were structurally skipped above.
            i = _match_angle(tokens, i)
        i += 1
    # A declaration immediately following a function body can be hidden from
    # the statement cursor by legacy FX syntax around the preceding block.  A
    # small top-level recovery pass makes precompiled shader objects explicit
    # without regex-removing arbitrary braces.  This is intentionally limited
    # to vertexshader/pixelshader declarations and deduplicates entries parsed
    # by the main structural walk.
    known = {(item.type.casefold(), item.name.casefold()) for item in effect.parameters}
    i = 0
    while i < len(tokens):
        lower = tokens[i].value.casefold()
        if lower in {"vertexshader", "pixelshader"} and i + 1 < len(tokens) and tokens[i + 1].kind == "ident":
            semi = _find_top_level(tokens, i, len(tokens), ";")
            if semi is not None:
                parsed = _parse_declaration(text, tokens[i:semi], matching, qualifiers)
                if parsed is not None:
                    parameter, _, ranges = parsed
                    if parameter is not None and (parameter.type.casefold(), parameter.name.casefold()) not in known:
                        effect.parameters.append(parameter)
                        effect.removable_ranges.extend(ranges)
                        known.add((parameter.type.casefold(), parameter.name.casefold()))
                i = semi + 1
                continue
        if tokens[i].value == "{" and i in matching:
            i = matching[i] + 1
            continue
        i += 1
    return effect


def _parse_struct_fields(text: str, tokens: list[Token], start: int, end: int) -> list[Parameter]:
    fields: list[Parameter] = []
    cursor = start
    while cursor < end:
        semi = _find_top_level(tokens, cursor, end, ";")
        if semi is None:
            break
        segment = tokens[cursor:semi]
        if len(segment) >= 2 and segment[0].kind == "ident" and segment[1].kind == "ident":
            semantic = None
            colon = next((idx for idx, tok in enumerate(segment) if tok.value == ":"), None)
            if colon is not None and colon + 1 < len(segment):
                semantic = segment[colon + 1].value
            fields.append(Parameter(segment[1].value, segment[0].value, semantic=semantic, line=segment[0].line))
        cursor = semi + 1
    return fields


def _parse_declaration(text: str, segment: list[Token], matching: dict[int, int], qualifiers: set[str]):
    del matching  # declaration-local grouping is handled by a fresh relative scan below
    if not segment or segment[0].value.casefold() in {"return", "discard"}:
        return None
    pos = 0
    qualifier_values = []
    while pos < len(segment) and segment[pos].value.casefold() in qualifiers:
        qualifier_values.append(segment[pos].value.casefold())
        pos += 1
    if pos + 1 >= len(segment) or segment[pos].kind != "ident" or segment[pos + 1].kind != "ident":
        return None
    type_name = segment[pos].value
    name_token = segment[pos + 1]
    name = name_token.value
    if pos + 2 < len(segment) and segment[pos + 2].value == "(":
        return None
    type_lower = type_name.casefold()
    ranges: list[tuple[int, int, str]] = []
    array_size = semantic = annotation = default = register = None
    default_equal: Optional[Token] = None
    i = pos + 2
    while i < len(segment):
        if segment[i].value == "[":
            depth = 1
            j = i + 1
            while j < len(segment) and depth:
                depth += (segment[j].value == "[") - (segment[j].value == "]")
                j += 1
            array_size = _slice(text, segment, i + 1, j - 1).strip()
            i = j
        elif segment[i].value == ":" and i + 1 < len(segment):
            if segment[i + 1].value.casefold() == "register":
                open_index = i + 2
                if open_index < len(segment) and segment[open_index].value == "(":
                    j = open_index + 1
                    depth = 1
                    while j < len(segment) and depth:
                        depth += (segment[j].value == "(") - (segment[j].value == ")")
                        j += 1
                    register = _slice(text, segment, open_index + 1, j - 1).strip()
                    ranges.append((segment[i].start, segment[j - 1].end, "register"))
                    i = j
                else:
                    i += 1
            else:
                semantic = segment[i + 1].value
                i += 2
        elif segment[i].value == "<":
            j = i + 1
            depth = 1
            while j < len(segment) and depth:
                depth += (segment[j].value == "<") - (segment[j].value == ">")
                j += 1
            annotation = _slice(text, segment, i + 1, j - 1).strip()
            ranges.append((segment[i].start, segment[j - 1].end, "annotation"))
            i = j
        elif segment[i].value == "=":
            default_equal = segment[i]
            default = _slice(text, segment, i + 1, len(segment)).strip()
            break
        else:
            i += 1
    if type_lower.startswith("sampler"):
        states = {}
        texture = None
        state_index = next((idx for idx, tok in enumerate(segment) if tok.value.casefold() == "sampler_state"), None)
        if state_index is not None:
            if state_index + 1 >= len(segment) or segment[state_index + 1].value != "{":
                raise FxError("SHD_SAMPLER_SYNTAX", f"sampler_state for {name} lacks a body", name_token.path, name_token.line, name_token.column)
            # Locate this declaration's brace structurally in its local token slice.
            depth = 1
            j = state_index + 2
            while j < len(segment) and depth:
                depth += (segment[j].value == "{") - (segment[j].value == "}")
                j += 1
            states = _assignments(text, segment, state_index + 2, j - 1)
            for key, value in states.items():
                if key.casefold() == "texture":
                    identifiers = [tok.value for tok in lex(value, name_token.path) if tok.kind == "ident"]
                    texture = identifiers[0] if identifiers else None
        ranges.append((segment[0].start, segment[-1].end, f"sampler:{name}"))
        return None, Sampler(name, type_name, texture, states, register, name_token.line), ranges
    parameter = Parameter(name, type_name, array_size, semantic, annotation, default, register, name_token.line)
    if type_lower in {"texture", "vertexshader", "pixelshader"} or (type_lower == "string" and name.startswith("_ALAMO_")):
        ranges.append((segment[0].start, segment[-1].end, f"non-hlsl:{type_lower}"))
    elif default is not None and "const" not in qualifier_values:
        assert default_equal is not None
        ranges.append((default_equal.start, segment[-1].end, "global-default"))
    return parameter, None, ranges


def _parse_passes(text: str, tokens: list[Token], matching: dict[int, int], start: int, end: int) -> list[Pass]:
    passes: list[Pass] = []
    i = start
    while i < end:
        if tokens[i].value.casefold() == "pass":
            token = tokens[i]
            cursor = i + 1
            name = f"p{len(passes)}"
            if cursor < end and tokens[cursor].kind == "ident":
                name = tokens[cursor].value
                cursor += 1
            if cursor < end and tokens[cursor].value == "<":
                cursor = _match_angle(tokens, cursor, end) + 1
            if cursor >= end or tokens[cursor].value != "{":
                raise FxError("SHD_PASS_SYNTAX", "pass requires a body", token.path, token.line, token.column)
            close = matching[cursor]
            states = _assignments(text, tokens, cursor + 1, close)
            vertex_key = next((key for key in states if key.casefold() == "vertexshader"), None)
            pixel_key = next((key for key in states if key.casefold() == "pixelshader"), None)
            vertex = _shader_assignment(states.pop(vertex_key)) if vertex_key else None
            pixel = _shader_assignment(states.pop(pixel_key)) if pixel_key else None
            draw = len(passes) == 0 or vertex is not None or pixel is not None or any("op[" in key.casefold() for key in states)
            passes.append(Pass(name, states, vertex, pixel, token.line, draw))
            i = close + 1
            continue
        i += 1
    return passes


def source_without_effect_wrappers(text: str, effect: Effect) -> str:
    """Remove only parser-identified effect wrappers, retaining executable HLSL."""

    ranges = sorted(effect.removable_ranges, key=lambda item: (item[0], item[1]))
    merged: list[tuple[int, int, str]] = []
    for start, end, reason in ranges:
        if merged and start < merged[-1][1]:
            # Sampler/default subranges can overlap their enclosing declaration.
            if end <= merged[-1][1]:
                continue
            raise ValueError(f"partially overlapping source transforms: {merged[-1]} and {(start, end, reason)}")
        merged.append((start, end, reason))
    pieces = []
    cursor = 0
    for start, end, reason in merged:
        pieces.append(text[cursor:start])
        removed = text[start:end]
        if reason.startswith("sampler:"):
            sampler_name = reason.split(":", 1)[1]
            sampler = next(item for item in effect.samplers if item.name == sampler_name)
            pieces.append(f"sampler2D {sampler.name}")
            pieces.append("\n" * removed.count("\n"))
        else:
            pieces.append("\n" * removed.count("\n"))
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces)


def prune_unreachable_functions(text: str, entry_points: Iterable[str]) -> str:
    """Retain the transitive function-call closure for the selected stages.

    Legacy shared headers contain helpers for unrelated materials that a modern
    compiler still parses eagerly.  This structure-aware reachability pass does
    not alter any retained function body; it removes whole, unreachable function
    definitions after building a token-level call graph.
    """
    tokens = [token for token in lex(text, "<lowered>") if token.kind not in {"directive", "eof"}]
    matching = _matching(tokens)
    functions: dict[str, list[tuple[int, int, set[str]]]] = {}
    i = 0
    statement_start = 0
    while i < len(tokens):
        token = tokens[i]
        if token.value == ";":
            statement_start = i + 1
        elif token.value == "{" and i in matching:
            close = matching[i]
            # Find the last closed parameter list between this top-level
            # declaration's start and body. A semantic may sit after it.
            close_paren = next((index for index in range(i - 1, statement_start - 1, -1)
                                if tokens[index].value == ")" and index in matching), None)
            is_function = False
            if close_paren is not None:
                open_paren = matching[close_paren]
                if open_paren > statement_start and tokens[open_paren - 1].kind == "ident":
                    name = tokens[open_paren - 1].value
                    calls = set()
                    for cursor in range(i + 1, close):
                        if (tokens[cursor].kind == "ident" and cursor + 1 < close
                                and tokens[cursor + 1].value == "("):
                            calls.add(tokens[cursor].value.casefold())
                    start_offset = tokens[statement_start].start if statement_start < len(tokens) else token.start
                    functions.setdefault(name.casefold(), []).append((start_offset, tokens[close].end, calls))
                    is_function = True
            i = close
            if is_function:
                statement_start = i + 1
        i += 1

    reachable = {name.casefold() for name in entry_points}
    pending = list(reachable)
    while pending:
        name = pending.pop()
        for _, _, calls in functions.get(name, []):
            for called in calls:
                if called in functions and called not in reachable:
                    reachable.add(called)
                    pending.append(called)
    removable = sorted((start, end) for name, definitions in functions.items() if name not in reachable
                       for start, end, _ in definitions)
    pieces: list[str] = []
    cursor = 0
    for start, end in removable:
        pieces.append(text[cursor:start])
        pieces.append("\n" * text[start:end].count("\n"))
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces)


def parameter_dict(parameter: Parameter) -> dict:
    return {
        "name": parameter.name,
        "type": parameter.type,
        "array_size": parameter.array_size,
        "semantic": parameter.semantic,
        "annotation": parameter.annotation,
        "default": parameter.default,
        "register": parameter.register,
        "line": parameter.line,
    }
