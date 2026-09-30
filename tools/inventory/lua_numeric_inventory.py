#!/usr/bin/env python3
"""Inventory numeric literals, operators and numeric library calls in Lua scripts.

Feeds the authoritative Lua numeric profile (#246): which conversions and
math the FoC scripts need, how literals behave under binary64 and under the
Q24 alternative (option A), and the operation mix of the space tactical AI
dependency closure (ai/spacemode plans plus their require() chains).
Counts are static token counts over the effective corpus, not runtime
frequencies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from fractions import Fraction
from pathlib import Path
from typing import Iterable

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from corpus import Corpus
else:
    from .corpus import Corpus

TOOL_VERSION = "1.0.0"
Q24_LIMIT = Fraction(1 << 39)
KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil",
    "not", "or", "repeat", "return", "then", "true", "until", "while",
}
OPERATORS = ["...", "..", "==", "~=", "<=", ">=", "<", ">", "=", "+", "-", "*", "/", "^", "(", ")", "{", "}",
             "[", "]", ";", ":", ",", "."]
NUMBER = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
BINARY_OPERATORS = {"+": "add", "-": "subtract", "*": "multiply", "/": "divide", "^": "power", "..": "concat",
                    "==": "equal", "~=": "not_equal", "<": "less", "<=": "less_equal", ">": "greater",
                    ">=": "greater_equal"}
UNARY_CONTEXT = set(BINARY_OPERATORS) | {"=", "(", "{", "[", ",", ";", "return", "and", "or", "not", "then",
                                          "do", "else", "elseif", "until", "in", None}
NUMERIC_CALLS = ("tostring", "tonumber", "string.format", "Dirty_Floor", "Simple_Mod", "GameRandom",
                 "GetCurrentTime", "Sleep", "Register_Timer")


class LexError(ValueError):
    pass


def long_bracket_end(text: str, start: int) -> int:
    # Lua 5.0 long brackets are [[ ... ]] and nest.
    depth = 0
    position = start
    while position < len(text):
        if text.startswith("[[", position):
            depth += 1
            position += 2
        elif text.startswith("]]", position):
            depth -= 1
            position += 2
            if depth == 0:
                return position
        else:
            position += 1
    raise LexError("unfinished long bracket")


def tokens(text: str) -> Iterable[tuple[str, str]]:
    position = 0
    size = len(text)
    while position < size:
        character = text[position]
        if character in " \t\r\n\v\f":
            position += 1
        elif text.startswith("--", position):
            if text.startswith("--[[", position):
                position = long_bracket_end(text, position + 2)
            else:
                end = text.find("\n", position)
                position = size if end < 0 else end + 1
        elif text.startswith("[[", position):
            position = long_bracket_end(text, position)
            yield "string", ""
        elif character in "\"'":
            cursor = position + 1
            while cursor < size and text[cursor] != character:
                cursor += 2 if text[cursor] == "\\" else 1
            if cursor >= size:
                raise LexError("unfinished string")
            yield "string", text[position + 1:cursor]
            position = cursor + 1
        elif character.isdigit() or (character == "." and position + 1 < size and text[position + 1].isdigit()):
            match = NUMBER.match(text, position)
            assert match is not None
            yield "number", match.group(0)
            position = match.end()
        elif character.isalpha() or character == "_":
            match = NAME.match(text, position)
            assert match is not None
            word = match.group(0)
            yield ("keyword" if word in KEYWORDS else "name"), word
            position = match.end()
        else:
            for operator in OPERATORS:
                if text.startswith(operator, position):
                    yield "operator", operator
                    position += len(operator)
                    break
            else:
                yield "other", character
                position += 1


def literal_facts(text: str) -> dict[str, object]:
    value = Fraction(text)
    binary64 = Fraction(float(text))
    q24 = value * (1 << 24)
    return {
        "integral": value.denominator == 1,
        "binary64_exact": binary64 == value,
        "q24_exact": q24.denominator == 1 and abs(value) < Q24_LIMIT,
        "q24_in_range": abs(value) < Q24_LIMIT,
    }


def analyze(text: str) -> tuple[Counter, Counter, set[str]]:
    counts: Counter = Counter()
    literals: Counter = Counter()
    requires: set[str] = set()
    previous: str | None = None
    stream = list(tokens(text))
    for index, (kind, value) in enumerate(stream):
        if kind == "number":
            counts["numeric_literal"] += 1
            literals[value] += 1
        elif kind == "operator" and value in BINARY_OPERATORS:
            if value == "-" and previous in UNARY_CONTEXT:
                counts["op_unary_minus"] += 1
            else:
                counts["op_" + BINARY_OPERATORS[value]] += 1
        elif kind == "keyword" and value == "for":
            if index + 2 < len(stream) and stream[index + 2] == ("operator", "="):
                counts["numeric_for"] += 1
            else:
                counts["generic_for"] += 1
        elif kind == "name":
            dotted = value
            cursor = index
            while cursor + 2 < len(stream) and stream[cursor + 1] == ("operator", ".") and stream[cursor + 2][0] == "name":
                dotted += "." + stream[cursor + 2][1]
                cursor += 2
            following = stream[cursor + 1] if cursor + 1 < len(stream) else None
            is_call = following in (("operator", "("),) or (following is not None and following[0] == "string")
            if previous != "." and is_call:
                if dotted in NUMERIC_CALLS or dotted.split(".")[0] == "math":
                    counts["call_" + dotted] += 1
                if dotted == "require" and following == ("operator", "(") and cursor + 2 < len(stream) and stream[cursor + 2][0] == "string":
                    requires.add(stream[cursor + 2][1].lower())
                elif dotted == "require" and following is not None and following[0] == "string":
                    requires.add(following[1].lower())
            elif previous != "." and dotted.split(".")[0] == "math":
                counts["reference_" + dotted] += 1
        if kind == "operator" or kind == "keyword":
            previous = value
        elif kind in ("name", "number", "string"):
            previous = kind
    return counts, literals, requires


def summarize(files: dict[str, tuple[Counter, Counter, set[str]]], names: Iterable[str]) -> dict[str, object]:
    names = sorted(names)
    counts: Counter = Counter()
    literals: Counter = Counter()
    for name in names:
        counts.update(files[name][0])
        literals.update(files[name][1])
    distinct = {}
    for text, count in sorted(literals.items(), key=lambda item: (Fraction(item[0]), item[0])):
        distinct[text] = {"count": count, **literal_facts(text)}
    facts = [(value, literals[text]) for text, value in distinct.items()]
    return {
        "file_count": len(names),
        "counts": dict(sorted(counts.items())),
        "literal_summary": {
            "distinct": len(distinct),
            "integral": sum(count for fact, count in facts if fact["integral"]),
            "fractional": sum(count for fact, count in facts if not fact["integral"]),
            "binary64_inexact": sum(count for fact, count in facts if not fact["binary64_exact"]),
            "q24_inexact": sum(count for fact, count in facts if not fact["q24_exact"]),
            "q24_out_of_range": sum(count for fact, count in facts if not fact["q24_in_range"]),
        },
        "distinct_literals": distinct,
    }


def closure(files: dict[str, tuple[Counter, Counter, set[str]]], seeds: Iterable[str]) -> set[str]:
    by_module: dict[str, list[str]] = {}
    for path in files:
        by_module.setdefault(Path(path).stem.lower(), []).append(path)
    pending = list(seeds)
    found: set[str] = set()
    while pending:
        path = pending.pop()
        if path in found:
            continue
        found.add(path)
        for module in files[path][2]:
            pending.extend(by_module.get(module, []))
    return found


def generate(game_root: Path) -> dict[str, object]:
    corpus = Corpus(game_root, [])
    sources = list(corpus.iter_sources("foc", ".lua", "effective"))
    files: dict[str, tuple[Counter, Counter, set[str]]] = {}
    identity = hashlib.sha256()
    for source in sources:
        identity.update(source.logical_path.encode("utf-8") + b"\0" + source.sha256.encode("ascii") + b"\n")
        files[source.logical_path] = analyze(source.data.decode("latin-1"))
    seeds = [path for path in files if path.startswith("data/scripts/ai/spacemode/")]
    space = closure(files, seeds)
    return {
        "schema_version": 1,
        "tool": "lua_numeric_inventory",
        "tool_version": TOOL_VERSION,
        "profile": "foc",
        "corpus": {"effective_files": len(files), "identity_sha256": identity.hexdigest()},
        "notes": "Static token counts. math.* appears only in comments; FoC's PGBase.lua implements "
                 "Dirty_Floor with string.format('%d') because no math library is exposed.",
        "foc_effective": summarize(files, files),
        "space_ai_closure": {"seeds": "data/scripts/ai/spacemode/*.lua plus require() chains",
                             "files": sorted(space), **summarize(files, space)},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = generate(args.game_root)
    args.out.write_text(json.dumps(result, indent=1, sort_keys=False) + "\n", encoding="utf-8", newline="\n")
    closure_counts = result["space_ai_closure"]["counts"]
    print(json.dumps({"foc": result["foc_effective"]["counts"], "space": closure_counts,
                      "space_literals": result["space_ai_closure"]["literal_summary"]}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
