"""Static call extraction for one Lua script (no execution) and the require closure of script roots."""
from __future__ import annotations

import dataclasses
import re
from pathlib import Path

from tools.inventory.lua_calls import LuaError, lex_lua, parse_source_calls

_REQUIRE = re.compile(r"""\b(?:require|dofile)\s*\(?\s*["']([^"']+)["']""")
_DEFINED = re.compile(r"^\s*(?:function\s+([A-Za-z_]\w*)|local\s+function\s+([A-Za-z_]\w*)|([A-Za-z_]\w*)\s*=)", re.M)
STANDARD_GLOBALS = {
    "assert", "collectgarbage", "dofile", "error", "gcinfo", "getfenv", "getmetatable", "ipairs", "loadfile", "loadlib",
    "loadstring", "next", "pairs", "pcall", "print", "rawequal", "rawget", "rawset", "require", "setfenv", "setmetatable",
    "tonumber", "tostring", "type", "unpack", "xpcall",
}
STANDARD_PREFIXES = {"coroutine", "debug", "io", "math", "os", "string", "table"}


@dataclasses.dataclass(frozen=True)
class Call:
    symbol: str
    kind: str  # global | dotted | method
    line: int
    function: str | None
    approx: bool = False


@dataclasses.dataclass
class Parsed:
    calls: list[Call]
    helpers: set[str]  # global function names the script defines
    defined: set[str]  # every top-level name the script assigns or defines
    requires: list[str]
    parse_error: str | None


def _decode(data: bytes) -> str:
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp1252", errors="replace")


def _approx_calls(text: str) -> list[Call]:
    """Token-level fallback for a script the strict parser rejects: name chains followed by a call."""
    toks = lex_lua(text)
    out: list[Call] = []
    for i, tok in enumerate(toks[:-1]):
        if tok.kind != "name":
            continue
        if i and toks[i - 1].value in {"function", ".", ":"}:
            continue
        chain, j, method = [tok.value], i + 1, False
        while j + 1 < len(toks) and toks[j].value in {".", ":"} and toks[j + 1].kind == "name":
            method = toks[j].value == ":"
            chain.append(toks[j + 1].value)
            j += 2
        nxt = toks[j] if j < len(toks) else None
        if nxt is None or nxt.value not in {"(", "{"} and nxt.kind != "string":
            continue
        if len(chain) == 1:
            out.append(Call(chain[0], "global", tok.line, None, True))
        elif method:
            out.append(Call(chain[-1], "method", tok.line, None, True))
        else:
            out.append(Call(".".join(chain), "dotted", tok.line, None, True))
    return out


def parse_script(data: bytes) -> Parsed:
    text = _decode(data)
    requires = [m.group(1).lower() for m in _REQUIRE.finditer(text)]
    defined = {a or b or c for a, b, c in _DEFINED.findall(text)}
    try:
        sites, helpers = parse_source_calls(data)
        # A method call on an expression the parser cannot name (a local variable, a call result) still names its method.
        calls = [Call(s.symbol, s.call_kind, s.line or 0, s.function) for s in sites
                 if s.symbol and (not s.reason or s.call_kind == "method")]
        error = None
    except (LuaError, RecursionError) as exc:
        calls, helpers, error = _approx_calls(text), set(), f"{type(exc).__name__}: {exc}"
        helpers = {m for m in re.findall(r"^\s*function\s+([A-Za-z_]\w*)\s*\(", text, re.M)}
    return Parsed(calls, set(helpers), defined, requires, error)


def is_standard(symbol: str, kind: str) -> bool:
    return (kind == "global" and symbol in STANDARD_GLOBALS) or (
        kind in {"dotted", "method"} and symbol.split(".", 1)[0] in STANDARD_PREFIXES)


def resolve_module(module: str, by_stem: dict[str, list[str]], search_dirs: list[str]) -> str | None:
    """Pick the script a require names: search-path order first, then the only candidate."""
    stem = Path(module.replace("\\", "/")).stem.lower()
    candidates = by_stem.get(stem, [])
    if not candidates:
        return None
    for directory in search_dirs:
        for candidate in candidates:
            if candidate.startswith(directory):
                return candidate
    return candidates[0] if len(candidates) == 1 else sorted(candidates)[0]


def closure(root: str, parsed: dict[str, Parsed], by_stem: dict[str, list[str]], search_dirs: list[str]) -> list[str]:
    """The root script plus every script its literal requires reach, root first, deterministic."""
    seen, todo, order = {root}, [root], [root]
    while todo:
        current = todo.pop()
        for module in parsed[current].requires:
            target = resolve_module(module, by_stem, search_dirs)
            if target and target in parsed and target not in seen:
                seen.add(target)
                order.append(target)
                todo.append(target)
    return order
