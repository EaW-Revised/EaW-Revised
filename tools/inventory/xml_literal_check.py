#!/usr/bin/env python3
"""Gate: source code must not copy a FoC XML value next to the name of its tag.

A hard-coded copy of an XML value (`double saturation_distance{300.0};  // Volume_Saturation_Distance`) goes stale
the moment a mod changes the XML and hides the value from the tag registry (docs/tag-coverage.md). This check scans
src/, include/ and apps/ for lines that name a tag of the registry (docs/tag-coverage/statuses.json) and carry a numeric
literal: the tag as a string or in a trailing comment on the same line, or in the comment block directly above a
declaration. Such a literal is accepted only when it is

  * a documented fallback: the line, or the comment line above it, carries `xml-fallback: <Tag>`, <Tag> is a registry tag,
    and within FALLBACK_WINDOW lines supported source patterns link its exact load key to a diagnostic
    on the missing branch. A header may instead cite
    `load: src/path.cpp#function`: supported source patterns must link an exact tag load key to a diagnostic
    on its missing branch (including local helpers, lambdas and tag tables); otherwise the line still fails, or
  * listed in the allowlist (config/xml-literal-allowlist.json): one entry per existing finding with its ticket. The
    allowlist may only shrink: an entry without a finding fails (delete it), and so does a list longer than `max_entries`.
    Growth is checked against the merge base (`--base-ref`, e.g. origin/Thom-Ernst/Skirmish): an entry the base list lacks fails
    unless the allowlist carries a new reviewed `growth_override` ({"ticket": <issue>, "reason": ...}). A replacement
    in the same file with the same registry tags, numeric literals (including conversion divisors), ticket and other
    metadata is a rename, not growth; each removed base entry covers at most one replacement. A new override that
    covers no growth fails; an unchanged inherited override is harmless but cannot approve later growth.
    Known limits: a tag on a continuation line of a multi-line declaration is missed, and
    the allowlist key is file + line text, so an identical line elsewhere in the same file is covered by one entry.

Usage: xml_literal_check.py check [--repo DIR] [--allowlist FILE] [--base-allowlist FILE | --base-ref REF]
       xml_literal_check.py scan  [--repo DIR]   (prints every finding as JSON, for building the allowlist)
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

if __package__ in (None, ""):
    from tag_registry import load_registry
else:
    from .tag_registry import load_registry

ROOTS = ("src", "include", "apps")
SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".gd")
SKIP_PARTS = ("/third_party/", "/upstream/", "/tests/")
FALLBACK_WINDOW = 20
_DIAGNOSTIC = re.compile(r"(?i)\b\w*(?:diagnos|warn|log|report|error|complain)\w*\s*\(|\bdiagnostics\s*\.\s*(?:push_back|emplace_back)\s*\(")
ALLOWLIST_PATH = "config/xml-literal-allowlist.json"
STATUSES_PATH = "docs/tag-coverage/statuses.json"
LOAD_REFERENCE = re.compile(r"\bload:\s*([A-Za-z_0-9/.-]+)#([A-Za-z_][A-Za-z_0-9]*)")
_COMMENT_OR_STRING = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|//[^\n]*|/\*.*?\*/', re.DOTALL)
MARKER = re.compile(r"xml-fallback:\s*([A-Za-z_][A-Za-z_0-9]*)")
_BLOCK_START = re.compile(r"\s*/\*(.*?)\*/(.*)$")

_WORD = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_STRING = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)\'')
_RULE_ID = re.compile(r"\b[A-Z]{1,6}-?[A-Z]?\d+(?:-\d+)?\b|#\d+|\bE\d+-\d+\b")
_NUMBER = re.compile(r"(?<![\w.])-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?[fFuUlL]*(?![\w.])")
_TRIVIAL = {"0", "1", "2", "-1", "0.0", "1.0", "2.0", "-1.0"}


def load_tags(repo: Path) -> set[str]:
    rows = load_registry(repo / STATUSES_PATH)["rows"]
    tags = set()
    for row in rows:
        tag = row["tag"]
        if tag == "*" or "/" in tag or "#" in tag:
            continue
        if len(tag) >= 9 and ("_" in tag or re.search(r"[a-z][A-Z]", tag)):
            tags.add(tag)
    return tags


def literals(code: str) -> list[str]:
    """Numeric literals of one code line, without those inside strings, identifiers, subscripts and template arguments."""
    text = _STRING.sub('""', code)
    text = re.sub(r"\[[^\]]*\]", "[]", text)
    text = re.sub(r"<[A-Za-z_:][^<>;()]*>", "<>", text)  # template arguments: std::array<float, 3>
    text = re.sub(r"case\s+-?\d+\s*:", " ", text)
    text = re.sub(r"\b[A-Za-z_]\w*\b", " ", text)
    found = [m.group(0).rstrip("fFuUlL") for m in _NUMBER.finditer(text)]
    return [value for value in found if value not in _TRIVIAL]


def split_comment(line: str) -> tuple[str, str]:
    in_string = None
    index = 0
    while index < len(line):
        char = line[index]
        if in_string:
            if char == "\\":
                index += 1
            elif char == in_string:
                in_string = None
        elif char in "\"'":
            in_string = char
        elif line.startswith("//", index):
            return line[:index], line[index:]
        index += 1
    return line, ""


def marker_is_backed(lines: list[str], index: int, marked: set[str], tags: set[str]) -> bool:
    """Use the explicit loader's missing-branch checks on the nearby source window."""
    lo, hi = max(0, index - FALLBACK_WINDOW), index + FALLBACK_WINDOW + 1
    text = source_without_comments("\n".join(lines[lo:hi]))
    definitions = local_callables(text)
    return any(missing_branch_is_backed(text, '"' + tag + '"', definitions)
               for tag in marked if tag in tags)


def source_without_comments(text: str) -> str:
    return _COMMENT_OR_STRING.sub(lambda match: " " if match.group().startswith(("//", "/*")) else match.group(), text)


def function_bodies(text: str, name: str) -> list[str]:
    """Find named definitions, balancing braces outside strings (all overloads)."""
    code = source_without_comments(text)
    masked = _STRING.sub(lambda match: " " * len(match.group()), code)
    signature = re.compile(r"^[ \t]*[\w:<>,*& \t]+\b" + re.escape(name)
                           + r"\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", re.MULTILINE)
    bodies = []
    for match in signature.finditer(masked):
        start = match.end() - 1
        depth = 1
        end = start + 1
        while end < len(masked) and depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        if depth == 0:
            bodies.append(code[start + 1:end - 1])
    return bodies


def masked_strings(text: str) -> str:
    return _STRING.sub(lambda match: " " * len(match.group()), text)


def balanced_end(text: str, start: int) -> int:
    """End of a parenthesized expression or block, with strings masked."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    stack = [pairs[text[start]]]
    for index in range(start + 1, len(text)):
        char = text[index]
        if char in pairs:
            stack.append(pairs[char])
        elif char == stack[-1]:
            stack.pop()
            if not stack:
                return index + 1
    return len(text)


def statement_end(masked: str, start: int) -> int:
    while start < len(masked) and masked[start].isspace():
        start += 1
    if start < len(masked) and masked[start] == "{":
        return balanced_end(masked, start)
    # Include nested if/else statements, rather than stopping at their first semicolon.
    conditional = re.match(r"if\s*\(", masked[start:])
    if conditional:
        condition_end = balanced_end(masked, start + conditional.end() - 1)
        end = statement_end(masked, condition_end)
        other = re.match(r"\s*else\b", masked[end:])
        return statement_end(masked, end + other.end()) if other else end
    for index in range(start, len(masked)):
        if masked[index] == ";":
            return index + 1
    return len(masked)


def local_callables(source: str) -> dict[str, list[tuple[list[str], str]]]:
    """Same-file definitions and local lambdas; this is deliberately not a C++ parser."""
    masked = masked_strings(source)
    pattern = re.compile(
        r"(?:^[ \t]*(?:\[\[[^\]]*\]\]\s*)?[\w:<>,*& \t]+\b(?P<function>[A-Za-z_]\w*)\s*"
        r"|\b(?:const\s+)?auto\s+(?P<lambda>[A-Za-z_]\w*)\s*=\s*\[[^\]]*\]\s*)"
        r"\((?P<parameters>[^;{}()]*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>,*&\s]+)?\{", re.MULTILINE)
    result: dict[str, list[tuple[list[str], str]]] = {}
    for match in pattern.finditer(masked):
        if match['function'] in {'if', 'for', 'while', 'switch', 'catch'}:
            continue
        parameters = match['parameters']
        while re.search(r"<[^<>]*>", parameters):
            parameters = re.sub(r"<[^<>]*>", "", parameters)
        params = [tokens[-1] for item in parameters.split(',') if (tokens := re.findall(r"[A-Za-z_]\w*", item))]
        start = match.end() - 1
        end = balanced_end(masked, start)
        result.setdefault(match['function'] or match['lambda'], []).append((params, source[start + 1:end - 1]))
    return result


def calls(text: str):
    """Yield call names and argument lists (commas inside nested calls are ignored)."""
    masked = masked_strings(text)
    for match in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:<[^<>;{}()]*>\s*)?\(", masked):
        start = match.end() - 1
        end = balanced_end(masked, start)
        args, left, index = [], start + 1, start + 1
        while index < end - 1:
            if masked[index] in "({[":
                index = balanced_end(masked, index)
                continue
            if masked[index] == ',':
                args.append(text[left:index].strip())
                left = index + 1
            index += 1
        args.append(text[left:end - 1].strip())
        yield match.group(1), args


_LOAD_CALLS = {"read", "has", "last_child", "number", "integer", "child", "find_child"}


def reads_key(text: str, key: str) -> bool:
    return any(name in _LOAD_CALLS and key in args for name, args in calls(text))


def emits(text: str, definitions: dict, seen: frozenset[str] = frozenset()) -> bool:
    if _DIAGNOSTIC.search(masked_strings(text)):
        return True
    return any(emits(body, definitions, seen | {name})
               for name, _ in calls(text) if name not in seen
               for _, body in definitions.get(name, []))


def missing_branch_is_backed(body: str, key: str, definitions: dict, seen: frozenset[str] = frozenset()) -> bool:
    """Follow supported load keys, missing branches, and parameterized local helpers."""
    masked = masked_strings(body)
    # Named tables used by range loops: substitute only inside that loop's body.
    for loop in re.finditer(r"\bfor\s*\(\s*const\s+auto(?:&)?\s+\[?(\w+)(?:\s*,\s*\w+\])?\s*:\s*(\w+)\s*\)", masked):
        variable, table = loop.groups()
        table_match = re.search(r"\b" + re.escape(table) + r"\s*\{", masked[:loop.start()])
        if table_match:
            table_start = table_match.end() - 1
            table_end = balanced_end(masked, table_start)
            if key in [m.group() for m in _STRING.finditer(body[table_start:table_end])]:
                end = statement_end(masked, loop.end())
                expanded = re.sub(r"\b" + re.escape(variable) + r"\b", lambda _: key, body[loop.end():end])
                if missing_branch_is_backed(expanded, key, definitions, seen):
                    return True
    for match in re.finditer(r"\bif\s*\(", masked):
        start = match.end() - 1
        end = balanced_end(masked, start)
        condition = body[start + 1:end - 1]
        then_end = statement_end(masked, end)
        branch = body[end:then_end]
        # A key read directly by a condition: !has(key) is the missing arm;
        # if (node = last_child(..., key)) handles absence in its else arm.
        load = reads_key(condition, key)
        negative = bool(re.search(r"!\s*(?![=])|==\s*nullptr", masked_strings(condition)))
        if not load:
            # const auto node = read(key); if (!node) ...
            for assignment in re.finditer(r"\b(\w+)\s*=\s*([^;]+);", body[:match.start()]):
                if reads_key(assignment.group(2), key) and re.fullmatch(
                        r"\s*(?:!\s*" + re.escape(assignment.group(1)) + r"|"
                        + re.escape(assignment.group(1)) + r"\s*==\s*nullptr)\s*", condition):
                    load, negative = True, True
        if load:
            if not negative:
                other = re.match(r"\s*else\b", masked[then_end:])
                if not other:
                    continue
                other_start = then_end + other.end()
                branch = body[other_start:statement_end(masked, other_start)]
            if emits(branch, definitions):
                return True
    # A helper called with this exact literal can own the read and missing branch.
    for name, args in calls(body):
        if key not in args or name in seen:
            continue
        for parameters, helper in definitions.get(name, []):
            if len(args) != len(parameters):
                continue
            for parameter, argument in zip(parameters, args):
                if argument == key:
                    helper = re.sub(r"\b" + re.escape(parameter) + r"\b", lambda _: key, helper)
            if missing_branch_is_backed(helper, key, definitions, seen | {name}):
                return True
    return False


def schema_field_keys(repo: Path) -> dict[str, str]:
    """Resolve command-bar Field keys from the ordered schema, not diagnostic messages."""
    try:
        header = source_without_comments((repo / "include/eawr/data/ui/command_bar.hpp").read_text(encoding="utf-8"))
        source = source_without_comments((repo / "src/data/ui/command_bar.cpp").read_text(encoding="utf-8"))
    except OSError:
        return {}
    enum = re.search(r"enum class Field\s*:[^{]+\{([^}]+)\}", header)
    schema = re.search(r"\bschema\s*\{\{(.*?)\}\};", source, re.DOTALL)
    if not enum or not schema:
        return {}
    fields = re.findall(r"\b([a-z_]\w*)\s*[,=]", enum[1])
    tags = re.findall(r'\{("[A-Za-z_]\w*"),\s*FieldKind::\w+\}', schema[1])
    return dict(zip(fields, tags))


def reference_is_backed(repo: Path, reference: tuple[str, str], marked: set[str], tags: set[str]) -> bool:
    rel, function = reference
    path = repo / rel
    if (Path(rel).is_absolute() or ".." in Path(rel).parts or not rel.startswith(tuple(root + "/" for root in ROOTS))
            or path.suffix not in SUFFIXES or any(part in "/" + rel for part in SKIP_PARTS)):
        return False
    try:
        path.resolve().relative_to(repo.resolve())
        source = source_without_comments(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    fields = schema_field_keys(repo)
    # Replace tokens only: a message or comment containing an enum name is not a load.
    enum_pattern = re.compile(r'"(?:[^"\\]|\\.)*"|\bdata::ui::Field::(\w+)\b')
    source = enum_pattern.sub(lambda m: fields.get(m[1], m[0]) if m[1] else m[0], source)
    definitions = local_callables(source)
    return any(missing_branch_is_backed(body, '"' + tag + '"', definitions)
               for body in function_bodies(source, function) for tag in marked if tag in tags)


def without_component_counts(code: str, definitions: dict) -> str:
    """Mask only the last integer argument of a local reader that checks its parsed list length."""
    masked = masked_strings(code)
    result = list(code)
    for match in re.finditer(r"\b(numbers|components)\s*\(", masked):
        end = balanced_end(masked, match.end() - 1)
        count = re.search(r",\s*(\d+[uUlL]*)\s*\)$", code[match.start():end])
        if not count:
            continue
        checked = any(parameters and re.search(
            r"\.size\s*\(\s*\)\s*(?:==|!=)\s*" + re.escape(parameters[-1]) + r"\b", body)
            for parameters, body in definitions.get(match[1], []))
        if checked:
            start = match.start() + count.start(1)
            result[start:match.start() + count.end(1)] = " " * len(count[1])
    return "".join(result)


def scan_file(path: Path, rel: str, tags: set[str], repo: Path | None = None) -> list[dict]:
    findings = []
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    source = source_without_comments("\n".join(lines))
    definitions = local_callables(source) if re.search(r"\b(?:numbers|components)\s*\(", source) else {}
    pending: tuple[set[str], set[str], tuple[str, str] | None] | None = None
    for number, raw in enumerate(lines, 1):
        block = _BLOCK_START.match(raw)
        if block:  # `/* Tag */ double d = 300.0;`: the comment names tags, the rest is code
            raw = f"{block.group(2)} //{block.group(1)}"
        code, comment = split_comment(raw)
        reference_match = LOAD_REFERENCE.search(comment)
        reference = reference_match.groups() if reference_match else None
        stripped = raw.strip()
        if not stripped:
            pending = None
            continue
        if stripped.startswith("//"):
            named = set(_WORD.findall(_RULE_ID.sub(" ", comment))) & tags
            fallback = set(MARKER.findall(comment))
            if pending is not None:
                named |= pending[0]
                fallback |= pending[1]
                reference = reference or pending[2]
            pending = (named, fallback, reference) if named or fallback else None
            continue
        if stripped.startswith(("/*", "*")):
            continue
        named = (set(_WORD.findall(_STRING.sub(lambda m: m.group(0).replace('"', " "), code))) | set(_WORD.findall(comment))) & tags
        fallback = set(MARKER.findall(comment))
        if pending is not None:
            named |= pending[0]
            fallback |= pending[1]
            reference = reference or pending[2]
            pending = None
        if not named:
            continue
        values = literals(without_component_counts(code, definitions))
        if not values:
            continue
        if fallback and (marker_is_backed(lines, number - 1, fallback, tags)
                         or (repo is not None and reference is not None
                             and reference_is_backed(repo, reference, fallback, tags))):
            continue
        findings.append(
            {"file": rel, "line": number, "tags": sorted(named), "literals": values, "text": " ".join(stripped.split()),
             "bad_marker": bool(fallback)}
        )
    return findings


def scan(repo: Path) -> list[dict]:
    tags = load_tags(repo)
    findings: list[dict] = []
    for root in ROOTS:
        base = repo / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            rel = path.relative_to(repo).as_posix()
            if path.suffix not in SUFFIXES or any(part in "/" + rel for part in SKIP_PARTS):
                continue
            findings.extend(scan_file(path, rel, tags, repo))
    return findings


def entry_key(item: dict) -> tuple[str, str]:
    return item["file"], item["text"]


def base_entries_from_git(repo: Path, ref: str, allowlist_rel: str = ALLOWLIST_PATH) -> dict | None:
    """The full allowlist at the merge base of HEAD and `ref`, or None when git, the ref or the file is not there."""

    def git(*args: str) -> str | None:
        try:
            done = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=False, timeout=60)
        except (OSError, subprocess.SubprocessError):
            return None
        return done.stdout if done.returncode == 0 else None

    base = git("merge-base", "HEAD", ref)
    shown = git("show", f"{base.strip()}:{allowlist_rel}") if base else None
    return json.loads(shown) if shown else None


def rename_key(entry: dict, tags: set[str]) -> tuple | None:
    """Conservative replacement identity; tagless lines still require their exact key."""
    named = tuple(sorted(set(_WORD.findall(entry["text"])) & tags))
    if not named:
        return None
    code, _ = split_comment(entry["text"])
    values = tuple(_NUMBER.findall(_STRING.sub('""', code)))
    metadata = {key: value for key, value in entry.items() if key != "text"}
    return named, values, json.dumps(metadata, sort_keys=True)


def check(repo: Path, allowlist_path: Path, base_allowlist: Path | list[dict] | dict | None) -> list[str]:
    problems: list[str] = []
    allow = json.loads(allowlist_path.read_text(encoding="utf-8"))
    entries = allow["entries"]
    for entry in entries:
        ticket = entry.get("ticket")
        if not (isinstance(ticket, int) or (ticket == "false-positive" and entry.get("reason"))):
            problems.append(f"allowlist entry needs an issue number, or ticket \"false-positive\" with a reason: {entry['file']}: {entry['text'][:60]}")
    if len(entries) > allow["max_entries"]:
        problems.append(f"allowlist has {len(entries)} entries, more than max_entries {allow['max_entries']} (it may only shrink)")
    allowed = {entry_key(entry) for entry in entries}
    found = scan(repo)
    seen = {entry_key(item) for item in found}
    for item in found:
        if entry_key(item) not in allowed:
            problems.append(
                f"{item['file']}:{item['line']}: names XML tag {', '.join(item['tags'])} next to literal "
                f"{', '.join(item['literals'])}; read the value from the XML, or mark a documented fallback with "
                f"`// xml-fallback: <Tag>` next to a load that reports a missing tag"
                + ("; the marker found is not backed by a load of that tag with a diagnostic nearby" if item.get("bad_marker") else "")
            )
    for entry in entries:
        if entry_key(entry) not in seen:
            problems.append(f"stale allowlist entry (finding fixed or line changed; delete it): {entry['file']}: {entry['text'][:60]}")
    if base_allowlist is not None:
        if isinstance(base_allowlist, list):
            base_allow = {"entries": base_allowlist}
        elif isinstance(base_allowlist, dict):
            base_allow = base_allowlist
        else:
            base_allow = json.loads(base_allowlist.read_text(encoding="utf-8"))
        base = base_allow["entries"]
        base_keys = {entry_key(entry) for entry in base}
        # Only removed entries can cover replacements; consume them once so renames cannot grow the list.
        tags = load_tags(repo)
        removed = [rename_key(entry, tags) for entry in base if entry_key(entry) not in allowed]
        grown = []
        for entry in entries:
            if entry_key(entry) in base_keys:
                continue
            identity = rename_key(entry, tags)
            if identity is not None and identity in removed:
                removed.remove(identity)
            else:
                grown.append(entry)
        override = allow.get("growth_override")
        approved = isinstance(override, dict) and isinstance(override.get("ticket"), int) and bool(override.get("reason"))
        new_override = override != base_allow.get("growth_override")
        if override is not None and not approved:
            problems.append('growth_override needs {"ticket": <issue number>, "reason": <text>}')
        if grown and not (approved and new_override):
            for entry in grown:
                problems.append(f"allowlist grew against the base (needs a reviewed growth_override): {entry['file']}: {entry['text'][:60]}")
        elif approved and new_override and not grown:
            problems.append("growth_override is set but the allowlist did not grow against the base; remove it")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("check", "scan"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--allowlist", type=Path)
    parser.add_argument("--base-allowlist", type=Path)
    parser.add_argument("--base-ref", help="git ref whose merge base with HEAD holds the allowlist that growth is compared against")
    args = parser.parse_args(argv)
    if args.command == "scan":
        print(json.dumps(scan(args.repo), indent=1))
        return 0
    allowlist = args.allowlist or args.repo / ALLOWLIST_PATH
    base = args.base_allowlist
    if base is None and args.base_ref:
        base = base_entries_from_git(args.repo, args.base_ref)
        if base is None:
            print(f"xml-literal gate: no allowlist at the merge base with {args.base_ref}; growth is not checked", file=sys.stderr)
    problems = check(args.repo, allowlist, base)
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"xml-literal gate: {'FAILED' if problems else 'ok'} ({len(problems)} problems)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
