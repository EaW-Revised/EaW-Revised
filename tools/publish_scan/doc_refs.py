#!/usr/bin/env python3
"""Check public Markdown for unresolved tracking, decision and rule references."""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
from pathlib import Path

ISSUE_REF = re.compile(r"(?<![\w#&.\\-])#(\d+)\b(?![0-9A-Za-z_-])")
LEGACY_REFS = re.compile(r"\(legacy #\d+(?:,\s*#\d+)*\)")
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")


def prose_mask(text: str) -> str:
    """Keep offsets and newlines while masking fenced and inline code and URL fragments."""
    chars = list(text)

    def mask(start: int, end: int) -> None:
        for index in range(start, end):
            if chars[index] not in "\r\n":
                chars[index] = " "

    fence: tuple[str, int] | None = None
    offset = 0
    for line in text.splitlines(keepends=True):
        found = FENCE.match(line)
        if fence:
            mask(offset, offset + len(line))
            if found and found[1][0] == fence[0] and len(found[1]) >= fence[1] and not found[2].strip():
                fence = None
        elif found and (found[1][0] == "~" or "`" not in found[2]):
            fence = (found[1][0], len(found[1]))
            mask(offset, offset + len(line))
        offset += len(line)

    visible = "".join(chars)
    runs = list(re.finditer(r"`+", visible))
    index = 0
    while index < len(runs):
        start = runs[index]
        closing = next((j for j in range(index + 1, len(runs)) if len(runs[j][0]) == len(start[0])), None)
        if closing is None:
            index += 1
        else:
            mask(start.start(), runs[closing].end())
            index = closing + 1
    # A numeric fragment in a link destination is an anchor, not an issue.
    visible = "".join(chars)
    for found in re.finditer(r"(?:https?://[^\s<>)]*|\]\([^\s)]*|^ {0,3}\[[^\]\n]+\]:\s*\S+)", visible, re.M):
        for fragment in re.finditer(r"#[^\s)]*", found[0]):
            mask(found.start() + fragment.start(), found.start() + fragment.end())
    return "".join(chars)


def bare_refs(text: str) -> list[tuple[int, str]]:
    masked = prose_mask(text)
    allowed = [(match.start(), match.end()) for match in LEGACY_REFS.finditer(masked)]
    return [(match.start(), match[0]) for match in ISSUE_REF.finditer(masked)
            if not any(start <= match.start() < end for start, end in allowed)]


def rewrite_legacy(text: str, prefix: str) -> tuple[str, int]:
    """Rewrite only explicit legacy groups in prose, preserving code byte for byte."""
    masked = prose_mask(text)
    matches = list(LEGACY_REFS.finditer(masked))
    count = sum(len(ISSUE_REF.findall(match[0])) for match in matches)
    for match in reversed(matches):
        text = text[:match.start()] + ISSUE_REF.sub(lambda m: f"{prefix}-{m[1]}", match[0]) + text[match.end():]
    return text, count


def exported_markdown(root: Path) -> list[tuple[Path, str]]:
    """Use private export exclusions when present; the public tree needs no private config."""
    config_path = root / "tools/oss/export.json"
    config = json.loads(config_path.read_text(encoding="utf-8")) if config_path.is_file() else {}
    patterns = [item["path"] for item in config.get("exclude", [])]
    blocks = set(config.get("always_strip_blocks", []))
    for switch, value in config.get("switches", {}).items():
        blocks.update(config.get("switch_blocks", {}).get(f"{switch}={value}", []))
    if (root / ".git").exists():
        output = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                cwd=root, capture_output=True, check=True).stdout
        names = sorted(set(output.decode("utf-8").split("\0")) - {""})
    else:
        names = sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())
    # Overlay docs are exported in place of their private counterparts. Check their source
    # text too, even though the maintainer overlay directory itself is excluded.
    overlay = root / config["overlay"] if config.get("overlay") else None
    replacements = {path.relative_to(overlay).as_posix(): path for path in overlay.rglob("*")
                    if path.is_file()} if overlay and overlay.is_dir() else {}
    sources = [(name, root / name) for name in names if name not in replacements]
    sources.extend((name, path) for name, path in replacements.items())
    result = []
    for name, path in sources:
        if path.suffix.lower() != ".md" or not path.is_file() or any(
                name.startswith(p) if p.endswith("/") else fnmatch.fnmatchcase(name, p) for p in patterns):
            continue
        chars = []
        active = None
        for line in path.read_text(encoding="utf-8").splitlines(keepends=True):
            marker = re.search(r"eawr-export:\s*([a-z0-9-]+)\s+(begin|end)\b", line)
            hidden = active in blocks
            if marker:
                active = marker[1] if marker[2] == "begin" else None
                hidden = True
            # The exporter drops list entries pointing to a file absent from the public tree.
            entry = re.match(r"^\s*[-*] \[[^\]]*\]\(([^)#\s]+)(?:#[^)]*)?\)", line)
            if entry:
                target = (root / name).parent / entry[1]
                try:
                    target_name = target.resolve().relative_to(root.resolve()).as_posix()
                except ValueError:
                    target_name = ""
                if target_name and any(target_name.startswith(p) if p.endswith("/")
                                       else fnmatch.fnmatchcase(target_name, p) for p in patterns):
                    hidden = True
            chars.append(re.sub(r"[^\r\n]", " ", line) if hidden else line)
        result.append((path, "".join(chars)))
    return result


# Public rule families whose definitions live in behaviour notes and plans.
# Evidence IDs (for example AU-/ESU-) name private observations, not public rules.
RULE_FAMILIES = ("WSU", "PU", "SK", "WBP", "WSQ", "WTA", "WWP", "WMV", "WFO",
                 "WCC", "WSS", "WBF", "WHE", "WHZ", "WPR", "WAB", "WR", "WU")
PUBLIC_ID = re.compile(r"\b(?:ADR-\d{3}|(?:" + "|".join(RULE_FAMILIES) +
                       r")-\d{2,3}|P[012]-\d{2}[a-z]?)\b")
LINK = re.compile(r"!?\[([^\]\n]*)\]\(([^\s)]*)\)")


def id_prose(text: str) -> str:
    """Exclude code and file paths; link labels remain reader-visible references."""
    chars = list(prose_mask(text))
    visible = "".join(chars)
    for match in re.finditer(r"\]\([^\n)]*\)|^ {0,3}\[[^\]\n]+\]:\s*\S+|"
                             r"(?:https?://[^\s<>)]*|[\w./-]+\.md)", visible, re.M):
        for index in range(match.start(), match.end()):
            if chars[index] not in "\r\n":
                chars[index] = " "
    return "".join(chars)


def defined_ids(path: Path, text: str) -> set[str]:
    """Only titled decisions, named plan entries and rule rows/bullets define IDs."""
    result = set()
    visible = id_prose(text)
    rule_table = False
    for line in visible.splitlines():
        if not line.lstrip().startswith("|"):
            rule_table = False
        else:
            cells = [cell.strip().lower() for cell in line.strip().strip("|").split("|")]
            if cells and cells[0] in ("rule", "rules", "id"):
                rule_table = len(cells) > 1 and bool(re.search(r"behaviour|\brule\b", cells[1]))
        decision = re.match(r"^#{1,6}\s+(ADR-\d{3}):\s+\S", line)
        if decision and path.name == "architecture-decisions.md":
            result.add(decision[1])
        if "plan" in path.parts:
            package = re.match(r"^\s*(?:#{1,6}\s+|[-*] \[.\] \*\*|\|\s*)"
                               r"(P[012]-\d{2}[a-z]?)(?::|\s)\s*\S", line)
            if package:
                result.add(package[1])
        if "behaviour" in path.parts or "plan" in path.parts:
            rule = re.match(r"^\s*(?:\|\s*|[-+]\s+(?:\*\*)?|#{1,6}\s+)"
                            r"([A-Z]+-\d{2,3})(?:\s*\||\*\*|[.:]|\s+\()\s*\S", line)
            if rule and (not line.lstrip().startswith("|") or rule_table):
                result.add(rule[1])
    return result


def id_refs(root: Path, sources: list[tuple[Path, str]]) -> list[str]:
    definitions = set().union(*(defined_ids(path, text) for path, text in sources))
    config_path = root / "tools/oss/export.json"
    config = json.loads(config_path.read_text(encoding="utf-8")) if config_path.is_file() else {}
    overlay = root / config["overlay"] if config.get("overlay") else None

    def public_path(path: Path) -> Path:
        if overlay and path.is_relative_to(overlay):
            return root / path.relative_to(overlay)
        return path

    anchors = {
        public_path(path).resolve(): {
            re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")
            for heading in re.findall(r"^#{1,6}\s+(ADR-\d{3}:.*)$", id_prose(text), re.M)}
        for path, text in sources if path.name == "architecture-decisions.md"
    }
    hits = []
    for path, text in sources:
        local = defined_ids(path, text)
        visible = id_prose(text)
        for match in PUBLIC_ID.finditer(visible):
            number = match[0]
            reason = None
            if number not in definitions:
                reason = "ID has no public decision or rule definition"
            elif re.match(r"P[012]-", number) and number not in local:
                reason = "describe the work instead of a standalone work-package ID"
            if reason:
                hits.append(f"{path.relative_to(root).as_posix()}:"
                            f"{text.count(chr(10), 0, match.start()) + 1}: {number}: {reason}")
        # ADR link fragments must actually name a decision heading, even when the
        # label describes the decision without repeating its ID.
        for match in LINK.finditer(text):
            if not visible[match.start():match.start() + 1].strip():
                continue
            target, separator, anchor = match[2].partition("#")
            if not separator or not anchor.startswith("adr-"):
                continue
            source_path = public_path(path)
            destination = (source_path.parent / target).resolve() if target else source_path.resolve()
            if anchor not in anchors.get(destination, set()):
                hits.append(f"{path.relative_to(root).as_posix()}:"
                            f"{text.count(chr(10), 0, match.start()) + 1}: unresolved decision anchor {match[2]}")
    return hits


def scan(root: Path) -> list[str]:
    sources = exported_markdown(root)
    return [f"{path.relative_to(root).as_posix()}:{text.count(chr(10), 0, offset) + 1}: "
            f"describe the work; keep tracking numbers only as (legacy {number})"
            for path, text in sources for offset, number in bare_refs(text)] + id_refs(root, sources)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    hits = scan(args.root.resolve())
    print("\n".join(hits) if hits else "Public Markdown references: clean")
    return int(bool(hits))


if __name__ == "__main__":
    raise SystemExit(main())
