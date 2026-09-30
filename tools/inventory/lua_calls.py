"""Lua 5.0 source parser and bounded PGLua static call analyzer.

Neither path executes scripts.  The source side is a real lexer plus recursive-
descent/Pratt parser; the compiled side decodes prototypes and propagates register
provenance over a control-flow graph.  Both intentionally preserve uncertainty.
"""
from __future__ import annotations

import dataclasses
import struct
from collections import deque
from typing import Any, Iterable

PGLUA_HEADER = bytes.fromhex("1b4c757051010404040608090908b6099368e7f57d41")
MAX_INPUT = 64 * 1024 * 1024
MAX_STRING = 16 * 1024 * 1024
MAX_DEPTH = 128
MAX_ENTRIES = 1_000_000


class LuaError(RuntimeError):
    def __init__(self, message: str, line: int | None = None, column: int | None = None,
                 offset: int | None = None, prototype: str | None = None):
        super().__init__(message)
        self.line, self.column, self.offset, self.prototype = line, column, offset, prototype


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    value: str
    line: int
    column: int


KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
            "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true",
            "until", "while"}
MULTI = ("...", "..", "==", "~=", "<=", ">=")


def lex_lua(text: str) -> list[Token]:
    out: list[Token] = []
    i, line, col = 0, 1, 1

    def advance(raw: str) -> None:
        nonlocal line, col
        pieces = raw.split("\n")
        if len(pieces) == 1:
            col += len(raw)
        else:
            line += len(pieces) - 1
            col = len(pieces[-1]) + 1

    while i < len(text):
        c = text[i]
        if c.isspace():
            advance(c); i += 1; continue
        if text.startswith("--", i):
            start = i
            if text.startswith("--[[", i):
                end = text.find("]]", i + 4)
                if end < 0:
                    raise LuaError("unterminated long comment", line, col)
                i = end + 2
            else:
                end = text.find("\n", i + 2)
                i = len(text) if end < 0 else end
            advance(text[start:i]); continue
        if text.startswith("[[", i):
            sl, sc, start = line, col, i
            end = text.find("]]", i + 2)
            if end < 0:
                raise LuaError("unterminated long string", line, col)
            i = end + 2; raw = text[start:i]; advance(raw)
            out.append(Token("string", raw, sl, sc)); continue
        if c in "'\"":
            sl, sc, quote, start = line, col, c, i
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    if i + 1 >= len(text):
                        raise LuaError("unterminated quoted string", sl, sc)
                    # Lua short strings may continue across a physical line only
                    # when the line ending is escaped.  Treat CRLF as one escaped
                    # newline; an unescaped CR or LF remains a lexical error.
                    if text[i + 1] == "\r":
                        i += 3 if i + 2 < len(text) and text[i + 2] == "\n" else 2
                    else:
                        i += 2
                    continue
                if text[i] in "\r\n":
                    raise LuaError("unescaped newline in quoted string", sl, sc)
                if i < len(text) and text[i] == quote:
                    i += 1; break
                i += 1
            else:
                raise LuaError("unterminated quoted string", sl, sc)
            raw = text[start:i]; advance(raw); out.append(Token("string", raw, sl, sc)); continue
        if c.isalpha() or c == "_":
            sl, sc, start = line, col, i
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] == "_"):
                i += 1
            raw = text[start:i]; advance(raw)
            out.append(Token("keyword" if raw in KEYWORDS else "name", raw, sl, sc)); continue
        if c.isdigit() or (c == "." and i + 1 < len(text) and text[i + 1].isdigit()):
            sl, sc, start = line, col, i
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] in ".+-"):
                # signs only legitimately follow exponent; the parser still rejects malformed adjacency.
                if text[i] in "+-" and text[i - 1] not in "eE": break
                i += 1
            raw = text[start:i]; advance(raw); out.append(Token("number", raw, sl, sc)); continue
        matched = next((op for op in MULTI if text.startswith(op, i)), None)
        if matched:
            out.append(Token("symbol", matched, line, col)); advance(matched); i += len(matched); continue
        if c in "+-*/^=<>;:,(){}[].%#":
            out.append(Token("symbol", c, line, col)); advance(c); i += 1; continue
        raise LuaError(f"unexpected character U+{ord(c):04X}", line, col)
    out.append(Token("eof", "<eof>", line, col))
    return out


@dataclasses.dataclass
class Expr:
    kind: str
    token: Token
    value: Any = None
    children: list["Expr"] = dataclasses.field(default_factory=list)


@dataclasses.dataclass(frozen=True)
class CallSite:
    symbol: str | None
    call_kind: str | None
    receiver: str | None
    line: int | None
    column: int | None
    prototype_id: int | None
    pc: int | None
    reason: str | None
    opcode: str


class LuaParser:
    PRECEDENCE = {"or": 1, "and": 2, "<": 3, ">": 3, "<=": 3, ">=": 3, "~=": 3,
                  "==": 3, "..": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6, "^": 8}

    def __init__(self, text: str):
        self.tokens = lex_lua(text)
        self.pos = 0
        self.calls: list[CallSite] = []
        self.scopes: list[dict[str, str | None]] = [{}]
        self.script_globals: set[str] = set()

    @property
    def tok(self) -> Token: return self.tokens[self.pos]

    def take(self, value: str | None = None) -> Token:
        token = self.tok
        if value is not None and token.value != value:
            raise LuaError(f"expected {value!r}, got {token.value!r}", token.line, token.column)
        self.pos += 1
        return token

    def accept(self, value: str) -> Token | None:
        if self.tok.value == value: return self.take()
        return None

    def name(self) -> Token:
        if self.tok.kind != "name":
            raise LuaError(f"expected name, got {self.tok.value!r}", self.tok.line, self.tok.column)
        return self.take()

    def parse(self) -> tuple[list[CallSite], set[str]]:
        self.block({"<eof>"})
        self.take("<eof>")
        return self.calls, self.script_globals

    def block(self, stop: set[str]) -> None:
        while self.tok.value not in stop:
            self.statement()
            self.accept(";")

    def scoped_block(self, stop: set[str]) -> None:
        self.scopes.append({})
        try: self.block(stop)
        finally: self.scopes.pop()

    def statement(self) -> None:
        value = self.tok.value
        if value == ";": self.take(); return
        if value == "if":
            self.take(); self.expression(); self.take("then"); self.scoped_block({"elseif", "else", "end"})
            while self.accept("elseif"):
                self.expression(); self.take("then"); self.scoped_block({"elseif", "else", "end"})
            if self.accept("else"): self.scoped_block({"end"})
            self.take("end"); return
        if value == "while":
            self.take(); self.expression(); self.take("do"); self.scoped_block({"end"}); self.take("end"); return
        if value == "repeat":
            self.take(); self.scoped_block({"until"}); self.take("until"); self.expression(); return
        if value == "do":
            self.take(); self.scoped_block({"end"}); self.take("end"); return
        if value == "for":
            self.take(); names = [self.name().value]
            if self.accept("="):
                self.expression(); self.take(","); self.expression()
                if self.accept(","): self.expression()
            else:
                while self.accept(","): names.append(self.name().value)
                self.take("in"); self.exprlist()
            self.take("do"); self.scopes.append({n: None for n in names})
            try: self.block({"end"})
            finally: self.scopes.pop()
            self.take("end"); return
        if value == "function":
            self.take(); base = self.name(); parts = [base.value]
            while self.accept("."): parts.append(self.name().value)
            method = self.accept(":")
            if method: parts.append(self.name().value)
            if len(parts) == 1: self.script_globals.add(parts[0])
            self.function_body("self" if method else None); return
        if value == "local":
            self.take()
            if self.accept("function"):
                token = self.name(); self.scopes[-1][token.value] = "<script>"
                self.function_body(None); return
            names = [self.name()]
            while self.accept(","): names.append(self.name())
            values: list[Expr] = []
            if self.accept("="): values = self.exprlist()
            for index, token in enumerate(names):
                self.scopes[-1][token.value] = self.alias_of(values[index]) if index < len(values) else None
            return
        if value == "return":
            self.take()
            if self.tok.value not in (";", "end", "else", "elseif", "until", "<eof>"): self.exprlist()
            return
        if value == "break": self.take(); return
        lhs = [self.prefix_expression()]
        if self.tok.value in ("=", ","):
            while self.accept(","): lhs.append(self.prefix_expression())
            self.take("="); rhs = self.exprlist()
            for index, target in enumerate(lhs): self.assign(target, rhs[index] if index < len(rhs) else None)
        elif lhs[0].kind != "call":
            raise LuaError("statement is neither assignment nor function call", lhs[0].token.line, lhs[0].token.column)

    def assign(self, target: Expr, value: Expr | None) -> None:
        if target.kind != "name": return
        alias = self.alias_of(value) if value else None
        for scope in reversed(self.scopes):
            if target.value in scope:
                scope[target.value] = alias; return
        if value and value.kind == "function": self.script_globals.add(target.value)

    def alias_of(self, expr: Expr | None) -> str | None:
        if expr is None: return None
        if expr.kind == "name":
            found, alias = self.lookup(expr.value)
            return alias if found else expr.value
        if expr.kind == "field":
            base = self.alias_of(expr.children[0])
            return f"{base}.{expr.value}" if base and base != "<script>" else None
        if expr.kind == "function": return "<script>"
        return None

    def lookup(self, name: str) -> tuple[bool, str | None]:
        for scope in reversed(self.scopes):
            if name in scope: return True, scope[name]
        return False, None

    def function_body(self, implicit: str | None) -> Expr:
        token = self.take("("); params: list[str] = []
        if self.tok.value != ")":
            if self.accept("..."): pass
            else:
                params.append(self.name().value)
                while self.accept(","):
                    if self.accept("..."): break
                    params.append(self.name().value)
        self.take(")")
        bindings = {p: None for p in params}
        if implicit: bindings[implicit] = None
        self.scopes.append(bindings)
        try: self.block({"end"})
        finally: self.scopes.pop()
        self.take("end")
        return Expr("function", token)

    def exprlist(self) -> list[Expr]:
        values = [self.expression()]
        while self.accept(","): values.append(self.expression())
        return values

    def expression(self, minimum: int = 0) -> Expr:
        if self.tok.value in ("not", "-", "#"):
            token = self.take(); left = Expr("unary", token, token.value, [self.expression(7)])
        else: left = self.atom()
        while self.tok.value in self.PRECEDENCE and self.PRECEDENCE[self.tok.value] >= minimum:
            token = self.take(); prec = self.PRECEDENCE[token.value]
            right = self.expression(prec + (0 if token.value in ("^", "..") else 1))
            left = Expr("binary", token, token.value, [left, right])
        return left

    def atom(self) -> Expr:
        token = self.tok
        if token.value == "function":
            self.take(); return self.function_body(None)
        if token.value == "{": return self.table()
        if token.kind in ("number", "string") or token.value in ("nil", "true", "false", "..."):
            self.take(); return Expr("literal", token, token.value)
        if token.kind == "name" or token.value == "(": return self.prefix_expression()
        raise LuaError(f"expected expression, got {token.value!r}", token.line, token.column)

    def table(self) -> Expr:
        token = self.take("{"); children: list[Expr] = []
        while self.tok.value != "}":
            if self.accept("["):
                children.append(self.expression()); self.take("]"); self.take("="); children.append(self.expression())
            elif self.tok.kind == "name" and self.tokens[self.pos + 1].value == "=":
                self.take(); self.take("="); children.append(self.expression())
            else: children.append(self.expression())
            if not (self.accept(",") or self.accept(";")): break
        self.take("}"); return Expr("table", token, children=children)

    def prefix_expression(self) -> Expr:
        if self.accept("("):
            value = self.expression(); self.take(")")
        else:
            token = self.name(); value = Expr("name", token, token.value)
        while True:
            if self.accept("."):
                member = self.name(); value = Expr("field", member, member.value, [value]); continue
            if self.accept("["):
                token = self.tokens[self.pos - 1]; key = self.expression(); self.take("]")
                value = Expr("index", token, children=[value, key]); continue
            if self.accept(":"):
                method = self.name(); self.arguments()
                value = self.record_call(Expr("method", method, method.value, [value]), method); continue
            if self.tok.value in ("(", "{") or self.tok.kind == "string":
                call_token = value.token; self.arguments(); value = self.record_call(value, call_token); continue
            break
        return value

    def arguments(self) -> list[Expr]:
        if self.accept("("):
            args = [] if self.tok.value == ")" else self.exprlist(); self.take(")"); return args
        if self.tok.value == "{": return [self.table()]
        return [Expr("literal", self.take(), "string")]

    def record_call(self, target: Expr, token: Token) -> Expr:
        symbol: str | None = None; kind: str | None = None; receiver: str | None = None; reason: str | None = None
        if target.kind == "name":
            found, alias = self.lookup(target.value)
            if found:
                if alias and alias != "<script>": symbol, kind = alias, "dotted" if "." in alias else "global"
                elif alias == "<script>": reason = "script-local function"
                else: reason = "dynamic or shadowed local callable"
            else: symbol, kind = target.value, "global"
        elif target.kind == "field":
            base = self.alias_of(target.children[0])
            if base: symbol, kind, receiver = f"{base}.{target.value}", "dotted", base
            else: reason = "dynamic dotted receiver"
        elif target.kind == "method":
            base = self.alias_of(target.children[0])
            symbol, kind, receiver = target.value, "method", base
            if base is None: reason = "dynamic method receiver"
        else: reason = "dynamic call expression"
        self.calls.append(CallSite(symbol, kind, receiver, token.line, token.column, None, None, reason, "source-call"))
        return Expr("call", token, children=[target])


def parse_source_calls(data: bytes) -> tuple[list[CallSite], set[str]]:
    # Lua source in this corpus is ASCII-compatible; reject undecodable bytes explicitly.
    try: text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        try: text = data.decode("cp1252")
        except UnicodeDecodeError as exc: raise LuaError(f"unsupported source encoding: {exc}") from exc
    return LuaParser(text).parse()


@dataclasses.dataclass
class Prototype:
    source: bytes | None
    first_line: int
    persistence_id: int
    upvalues: int
    parameters: int
    vararg: int
    max_stack: int
    lines: list[int]
    locals: list[tuple[bytes | None, int, int]]
    upvalue_names: list[bytes | None]
    constants: list[Any]
    prototypes: list["Prototype"]
    code: list[int]
    path: str


class PGLuaDecoder:
    def __init__(self, data: bytes):
        if len(data) > MAX_INPUT: raise LuaError("PGLua input exceeds 64 MiB", offset=0)
        self.data, self.offset = data, 0
        self.totals = {k: 0 for k in ("instruction", "constant", "prototype", "local", "upvalue", "line")}
        self.ids: set[int] = set(); self.next_id = 1

    def read(self, size: int, field: str, path: str) -> bytes:
        if size < 0 or self.offset + size > len(self.data):
            raise LuaError(f"truncated {field}", offset=self.offset, prototype=path)
        out = self.data[self.offset:self.offset + size]; self.offset += size; return out

    def u8(self, field: str, path: str) -> int: return self.read(1, field, path)[0]
    def i32(self, field: str, path: str) -> int: return struct.unpack("<i", self.read(4, field, path))[0]
    def u32(self, field: str, path: str) -> int: return struct.unpack("<I", self.read(4, field, path))[0]

    def count(self, category: str, field: str, path: str) -> int:
        count = self.i32(field, path)
        if count < 0: raise LuaError(f"negative {field}", offset=self.offset - 4, prototype=path)
        self.totals[category] += count
        if self.totals[category] > MAX_ENTRIES: raise LuaError(f"{category} aggregate limit exceeded", offset=self.offset - 4, prototype=path)
        return count

    def string(self, field: str, path: str) -> bytes | None:
        size = self.u32(field + " length", path)
        if size == 0: return None
        if size > MAX_STRING: raise LuaError(f"{field} exceeds 16 MiB", offset=self.offset - 4, prototype=path)
        value = self.read(size, field, path)
        if value[-1] != 0: raise LuaError(f"{field} lacks trailing NUL", offset=self.offset - 1, prototype=path)
        return value[:-1]

    def prototype(self, depth: int, path: str, inherited: bytes | None) -> Prototype:
        if depth > MAX_DEPTH: raise LuaError("prototype depth limit exceeded", offset=self.offset, prototype=path)
        source = self.string("source", path) or inherited
        first = self.i32("first source line", path)
        if first < 0: raise LuaError("negative first source line", offset=self.offset - 4, prototype=path)
        pid = self.i32("persistence identifier", path)
        if pid <= 0 or pid in self.ids or pid != self.next_id:
            raise LuaError("persistence identifiers are not positive depth-first sequence", offset=self.offset - 4, prototype=path)
        self.ids.add(pid); self.next_id += 1; self.totals["prototype"] += 1
        if self.totals["prototype"] > MAX_ENTRIES: raise LuaError("prototype aggregate limit exceeded", offset=self.offset, prototype=path)
        upvalues = self.u8("upvalue count", path); parameters = self.u8("parameter count", path)
        vararg = self.u8("vararg flag", path); max_stack = self.u8("maximum stack", path)
        if max_stack == 0 or max_stack > 250: raise LuaError("invalid maximum stack", offset=self.offset - 1, prototype=path)
        lines = [self.i32("source line", path) for _ in range(self.count("line", "line count", path))]
        locals_: list[tuple[bytes | None, int, int]] = []
        for _ in range(self.count("local", "local count", path)):
            name = self.string("local name", path); start = self.i32("local start pc", path); end = self.i32("local end pc", path)
            if start < 0 or end < start: raise LuaError("invalid local PC range", offset=self.offset - 8, prototype=path)
            locals_.append((name, start, end))
        names = [self.string("upvalue name", path) for _ in range(self.count("upvalue", "upvalue-name count", path))]
        constants: list[Any] = []
        for _ in range(self.count("constant", "constant count", path)):
            tag = self.u8("constant tag", path)
            if tag == 0: constants.append(None)
            elif tag == 3: constants.append(struct.unpack("<d", self.read(8, "number constant", path))[0])
            elif tag == 4: constants.append(self.string("string constant", path))
            else: raise LuaError(f"unsupported constant tag {tag}", offset=self.offset - 1, prototype=path)
        nested_count = self.count("prototype", "nested prototype count", path)
        # The count above reserves quota; avoid double counting in recursive records.
        self.totals["prototype"] -= nested_count
        nested = [self.prototype(depth + 1, f"{path}.{i}", source) for i in range(nested_count)]
        code = [self.u32("instruction", path) for _ in range(self.count("instruction", "instruction count", path))]
        proto = Prototype(source, first, pid, upvalues, parameters, vararg, max_stack, lines, locals_, names, constants, nested, code, path)
        validate_prototype(proto)
        return proto

    def decode(self) -> Prototype:
        if self.read(len(PGLUA_HEADER), "header", "root") != PGLUA_HEADER:
            raise LuaError("header does not match pinned PGLua contract", offset=0, prototype="root")
        root = self.prototype(1, "0", None)
        if self.offset != len(self.data): raise LuaError("trailing bytes after root prototype", offset=self.offset, prototype="0")
        return root


OPNAMES = ("MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
           "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM", "NOT",
           "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "CALL", "TAILCALL", "RETURN", "FORLOOP", "TFORLOOP",
           "TFORPREP", "SETLIST", "SETLISTO", "CLOSE", "CLOSURE")


def fields(word: int) -> tuple[int, int, int, int, int, int]:
    op = word & 0x3F; c = (word >> 6) & 0x1FF; b = (word >> 15) & 0x1FF; a = (word >> 24) & 0xFF
    bx = (word >> 6) & 0x3FFFF; return op, a, b, c, bx, bx - 131071


def validate_prototype(proto: Prototype) -> None:
    def register(value: int, pc: int, label: str) -> None:
        if value >= proto.max_stack:
            raise LuaError(f"register {label} out of range at PC {pc}", prototype=proto.path)

    for pc, word in enumerate(proto.code):
        op, a, b, c, bx, sbx = fields(word)
        if op >= len(OPNAMES): raise LuaError(f"unsupported opcode {op}", prototype=proto.path, offset=pc)
        if a >= proto.max_stack and op not in (20, 27, 33): raise LuaError(f"register A out of range at PC {pc}", prototype=proto.path)
        if op in (1, 5, 7) and bx >= len(proto.constants): raise LuaError(f"constant index out of range at PC {pc}", prototype=proto.path)
        if op == 34 and bx >= len(proto.prototypes): raise LuaError(f"prototype index out of range at PC {pc}", prototype=proto.path)
        if op in (20, 28, 30) and not (0 <= pc + 1 + sbx < len(proto.code)):
            raise LuaError(f"jump target out of range at PC {pc}", prototype=proto.path)
        for operand in ((b, c) if op in (6, 9, 11, 12, 13, 14, 15, 16, 21, 22, 23) else ()):
            if operand < 250 and operand >= proto.max_stack: raise LuaError(f"RK register out of range at PC {pc}", prototype=proto.path)
        if op == 0: register(b, pc, "B")
        elif op == 3: register(b, pc, "B")
        elif op == 4 and b >= proto.upvalues: raise LuaError(f"upvalue out of range at PC {pc}", prototype=proto.path)
        elif op == 6: register(b, pc, "B")
        elif op == 8 and b >= proto.upvalues: raise LuaError(f"upvalue out of range at PC {pc}", prototype=proto.path)
        elif op == 11:
            register(a + 1, pc, "A+1"); register(b, pc, "B")
        elif op in (17, 18): register(b, pc, "B")
        elif op == 19:
            register(b, pc, "B"); register(c, pc, "C")
            if b > c: raise LuaError(f"invalid CONCAT range at PC {pc}", prototype=proto.path)
        elif op in (25, 26):
            if b and a + b - 1 >= proto.max_stack: raise LuaError(f"call argument range out of bounds at PC {pc}", prototype=proto.path)
            if op == 25 and c > 1 and a + c - 2 >= proto.max_stack: raise LuaError(f"call result range out of bounds at PC {pc}", prototype=proto.path)
        elif op == 27 and b > 1 and a + b - 2 >= proto.max_stack:
            raise LuaError(f"return range out of bounds at PC {pc}", prototype=proto.path)
        elif op in (28, 30): register(a + 2, pc, "A+2")
    if proto.lines and len(proto.lines) != len(proto.code):
        raise LuaError("line-info count differs from instruction count", prototype=proto.path)
    for _name, start, end in proto.locals:
        if start > len(proto.code) or end > len(proto.code):
            raise LuaError("local PC range exceeds instruction vector", prototype=proto.path)


@dataclasses.dataclass(frozen=True)
class Prov:
    kind: str
    value: str | None = None
    base: str | None = None

UNKNOWN = Prov("unknown")


def _constant_string(proto: Prototype, index: int) -> str | None:
    if not (0 <= index < len(proto.constants)): return None
    value = proto.constants[index]
    if not isinstance(value, bytes): return None
    return value.decode("latin-1")


def analyze_pglua(data: bytes) -> tuple[list[CallSite], dict[str, Any]]:
    root = PGLuaDecoder(data).decode()
    calls: list[CallSite] = []; unsupported: list[dict[str, Any]] = []; prototype_count = 0; instruction_count = 0

    def analyze(proto: Prototype) -> None:
        nonlocal prototype_count, instruction_count
        prototype_count += 1; instruction_count += len(proto.code)
        initial = tuple(UNKNOWN for _ in range(proto.max_stack))
        states: dict[int, tuple[Prov, ...]] = {0: initial} if proto.code else {}
        queue = deque([0] if proto.code else [])
        call_observations: dict[int, set[Prov]] = {}

        def merge(old: tuple[Prov, ...] | None, new: tuple[Prov, ...]) -> tuple[Prov, ...]:
            if old is None: return new
            return tuple(a if a == b else UNKNOWN for a, b in zip(old, new))

        while queue:
            pc = queue.popleft(); state = list(states[pc]); op, a, b, c, bx, sbx = fields(proto.code[pc])
            if op in (25, 26): call_observations.setdefault(pc, set()).add(state[a])
            def rk(value: int) -> Prov:
                if value < 250: return state[value]
                literal = _constant_string(proto, value - 250)
                return Prov("literal", literal) if literal is not None else UNKNOWN
            if op == 0: state[a] = state[b]
            elif op == 1: state[a] = Prov("literal", _constant_string(proto, bx)) if _constant_string(proto, bx) is not None else UNKNOWN
            elif op == 3:
                # LOADNIL A B assigns nil to the complete inclusive range.
                for destination in range(a, b + 1):
                    state[destination] = UNKNOWN
            elif op in (2, 4, 10, 12, 13, 14, 15, 16, 17, 18, 19, 29): state[a] = UNKNOWN
            elif op == 5:
                name = _constant_string(proto, bx); state[a] = Prov("global", name) if name is not None else UNKNOWN
            elif op == 6:
                key = rk(c); base = state[b]
                if key.kind == "literal" and key.value is not None:
                    base_name = base.value if base.kind in ("global", "field") else None
                    state[a] = Prov("field", key.value, base_name)
                else: state[a] = UNKNOWN
            elif op == 11:
                key = rk(c); base = state[b]
                state[a + 1] = base
                state[a] = Prov("field", key.value, base.value if base.kind in ("global", "field") else None) if key.kind == "literal" else UNKNOWN
            elif op == 34: state[a] = Prov("closure", str(proto.prototypes[bx].persistence_id))
            elif op in (25, 26):
                # CALL A B C writes C-1 results beginning at A.  C == 0 is
                # the open-result form, so no finite upper destination is
                # encoded; conservatively invalidate through max_stack.  The
                # same transfer is applied to TAILCALL even though its normal
                # control-flow has no successor, keeping the opcode semantics
                # honest if successor modeling is extended later.
                result_end = proto.max_stack if c == 0 else a + max(c - 1, 0)
                for destination in range(a, result_end):
                    state[destination] = UNKNOWN
            if op in (28, 32): unsupported.append({"opcode": OPNAMES[op], "pc": pc, "prototype_id": proto.persistence_id})
            successors: list[int]
            if op in (27, 26): successors = []
            elif op == 20: successors = [pc + 1 + sbx]
            elif op in (21, 22, 23, 24, 29): successors = [pc + 1, pc + 2]
            elif op in (28, 30): successors = [pc + 1, pc + 1 + sbx]
            else: successors = [pc + 1]
            next_state = tuple(state)
            for successor in successors:
                if not (0 <= successor < len(proto.code)): continue
                merged = merge(states.get(successor), next_state)
                if states.get(successor) != merged:
                    states[successor] = merged; queue.append(successor)
        for pc in sorted(call_observations):
            values = call_observations[pc]
            prov = next(iter(values)) if len(values) == 1 else UNKNOWN
            opcode = OPNAMES[fields(proto.code[pc])[0]]
            if prov.kind == "global":
                calls.append(CallSite(prov.value, "global", None, None, None, proto.persistence_id, pc, None, opcode))
            elif prov.kind == "field":
                reason = None if prov.base else "unknown receiver provenance"
                calls.append(CallSite(prov.value, "method", prov.base, None, None, proto.persistence_id, pc, reason, opcode))
            else:
                reason = "merged control-flow callable" if len(values) > 1 else f"{prov.kind} callable provenance"
                calls.append(CallSite(None, None, None, None, None, proto.persistence_id, pc, reason, opcode))
        for child in proto.prototypes: analyze(child)
    analyze(root)
    return calls, {"instruction_count": instruction_count, "prototype_count": prototype_count,
                   "unsupported_instructions": sorted(unsupported, key=lambda x: (x["prototype_id"], x["pc"]))}
