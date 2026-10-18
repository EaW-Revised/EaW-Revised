#!/usr/bin/env python3
"""Check docs/registries/replay-numbers.json against docs/replay-format.md and the code constants.

The registry lists the tactical replay numbers (format versions, header extension tags, command
opcodes) and the tactical state-block tags. Three-way rules:

* code <-> registry: every constant the code defines is registered with the same value, and every
  registered row that names a constant finds it with that value. Rows without a constant (opcodes
  1-11 are literals in the parser) carry `"constant": null`.
* doc <-> registry: each row's `documented` flag equals whether docs/replay-format.md mentions it
  (opcode table row, `Tag N` / the four-letter name, block tag word). A documentation gap is
  recorded as `"documented": false`; closing the gap in the doc then fails the check until the
  flag is flipped, so the registry never drifts from the doc.
* doc-only numbers (an opcode row or a `Tag N` mention the registry lacks) are reported too.

Scope: the tactical replay (v2-v5) only. The v1 synthetic section, the galactic replay and
checkpoint constants, and opcode literals other than the 1-11 doc rows are not covered.

Usage: python tools/check_replay_numbers.py [--root DIR] [--print]
`--print` dumps the numbers found in the code, to help add new rows. Exit 1 on any mismatch.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REGISTRY = "docs/registries/replay-numbers.json"
DOC = "docs/replay-format.md"
REPLAY_HEADER = "include/eawr/sim/tactical/replay.hpp"
OPCODE_HEADER = "src/sim/tactical/tactical_internal.hpp"
BLOCK_DIRS = ("src/sim/tactical", "include/eawr/sim/tactical")

VERSION_RE = re.compile(r"constexpr\s+std::uint16_t\s+(replay_format_version\w*)\s*=\s*(\d+)\s*;")
EXTENSION_RE = re.compile(r"constexpr\s+std::uint16_t\s+(replay_extension_\w+)\s*=\s*(\d+)\s*;")
OPCODE_RE = re.compile(r"constexpr\s+std::uint8_t\s+(opcode_\w+)\s*=\s*(\d+)\s*;")
OPCODE_ALIAS_RE = re.compile(r"constexpr\s+std::uint8_t\s+(max_command_opcode)\s*=\s*(\w+)\s*;")
TAG_MENTION = r"(?<![Aa]bsent )(?<![Nn]o )\b[Tt]ag {}\b"
TAG_ANY_RE = re.compile(r"(?<![Aa]bsent )(?<![Nn]o )\b[Tt]ag (\d+)\b")
CHARS = r"'(\w)'\s*,\s*'(\w)'\s*,\s*'(\w)'\s*,\s*'(\w)'"
BLOCK_RES = (
    re.compile(r"std::array<std::uint8_t,\s*4>\s+(\w+)\s*\{\s*" + CHARS + r"\s*\}"),
    re.compile(r"std::uint8_t\s+(tag)\[\]\s*=\s*\{\s*" + CHARS + r"\s*\}"),
)


def read(root, relative):
    return (root / relative).read_text(encoding="utf-8")


def code_facts(root):
    """Return (versions, extensions, opcodes, max_opcode, blocks) read from the code."""
    header = read(root, REPLAY_HEADER)
    versions = {name: int(value) for name, value in VERSION_RE.findall(header)}
    extensions = {name: int(value) for name, value in EXTENSION_RE.findall(header)}
    internal = read(root, OPCODE_HEADER)
    opcodes = {name: int(value) for name, value in OPCODE_RE.findall(internal)}
    alias = OPCODE_ALIAS_RE.search(internal)
    max_opcode = (int(alias.group(2)) if alias.group(2).isdigit() else opcodes.get(alias.group(2))) if alias else None
    blocks = {}
    for directory in BLOCK_DIRS:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".cpp", ".hpp"):
                continue
            for pattern in BLOCK_RES:
                for match in pattern.finditer(path.read_text(encoding="utf-8", errors="replace")):
                    blocks.setdefault("".join(match.groups()[1:]), set()).add(match.group(1))
    return versions, extensions, opcodes, max_opcode, blocks


def doc_facts(text):
    """Return the opcode numbers of the v2 command table and the doc text without the v1 section."""
    start = text.index("# Tactical replay format v2")
    tactical = text[start:]
    commands = tactical.index("## Commands")
    table = tactical[commands:tactical.index("\n\n", tactical.index("| Opcode |"))]
    opcodes = {int(row.group(1)) for row in re.finditer(r"^\|\s*(\d+)\s*\|", table, re.M)}
    return opcodes, tactical


def check(root):
    problems = []
    registry = json.loads(read(root, REGISTRY))
    versions, extensions, opcodes, max_opcode, blocks = code_facts(root)
    doc_opcodes, doc = doc_facts(read(root, DOC))

    def compare(kind, rows, code, key):
        listed = {}
        for row in rows:
            name = row.get("constant")
            if row[key] in listed:
                problems.append(f"{kind} {row[key]} is registered twice")
            listed[row[key]] = row
            if name is None:
                continue
            if name not in code:
                problems.append(f"{kind} {row[key]} names constant {name}, which the code does not define")
            elif code[name] != row[key]:
                problems.append(f"{kind} {name}: registry {row[key]}, code {code[name]}")
        for name, value in code.items():
            if value not in listed:
                problems.append(f"{kind} {name} = {value} is in the code but not in the registry")
            elif listed[value].get("constant") != name:
                problems.append(f"{kind} {value}: code constant {name}, registry {listed[value].get('constant')}")
        return listed

    compare("format version", registry["tactical_replay"]["format_versions"], versions, "value")
    ext_rows = compare("header extension", registry["tactical_replay"]["header_extensions"], extensions, "tag")
    op_rows = compare("opcode", registry["tactical_replay"]["opcodes"], opcodes, "value")
    if max_opcode is None:
        problems.append("max_command_opcode is missing or not a number or opcode constant")
    elif max_opcode != max(op_rows):
        problems.append(f"max_command_opcode is {max_opcode}, the highest registered opcode is {max(op_rows)}")

    for tag, row in ext_rows.items():
        mentioned = bool(re.search(TAG_MENTION.format(tag), doc)) or bool(row.get("name") and f"`{row['name']}`" in doc)
        if mentioned != row["documented"]:
            problems.append(f"header extension {tag}: documented={row['documented']}, the doc says {mentioned}")
    for value, row in op_rows.items():
        mentioned = value in doc_opcodes
        if mentioned != row["documented"]:
            problems.append(f"opcode {value}: documented={row['documented']}, the doc table says {mentioned}")

    for value in sorted(doc_opcodes - set(op_rows)):
        problems.append(f"opcode {value} is in the doc's command table but not in the registry")
    for tag in sorted({int(found) for found in TAG_ANY_RE.findall(doc)} - set(ext_rows)):
        problems.append(f"header extension tag {tag} is mentioned in the doc but not in the registry")

    block_rows = {}
    for row in registry["tactical_state_blocks"]:
        if row["tag"] in block_rows:
            problems.append(f"state block {row['tag']} is registered twice")
        block_rows[row["tag"]] = row
        constants = blocks.get(row["tag"])
        if constants is None:
            problems.append(f"state block {row['tag']} is registered but the code defines no such tag")
        elif row["constant"] not in constants:
            problems.append(f"state block {row['tag']}: registry constant {row['constant']}, code {sorted(constants)}")
        mentioned = bool(re.search(rf"\b{row['tag']}\b", doc))
        if mentioned != row["documented"]:
            problems.append(f"state block {row['tag']}: documented={row['documented']}, the doc says {mentioned}")
    for tag, constants in blocks.items():
        if tag not in block_rows:
            problems.append(f"state block {tag} ({sorted(constants)}) is in the code but not in the registry")

    return problems


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--print", action="store_true", dest="dump")
    args = parser.parse_args(argv)
    root = Path(args.root)
    if args.dump:
        versions, extensions, opcodes, max_opcode, blocks = code_facts(root)
        print(json.dumps({"versions": versions, "extensions": extensions, "opcodes": opcodes,
                          "max_opcode": max_opcode, "blocks": {k: sorted(v) for k, v in blocks.items()}}, indent=1))
        return 0
    problems = check(root)
    for problem in problems:
        print(f"check_replay_numbers: {problem}")
    if problems:
        print(f"check_replay_numbers: {len(problems)} mismatch(es) between {REGISTRY}, {DOC} and the code")
        return 1
    print("check_replay_numbers: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
