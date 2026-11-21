"""Which GameObject scripts are attached to galactic objects, from the Lua_Script tags of the XML data."""
from __future__ import annotations

import os
import re
import xml.etree.ElementTree as ET
from pathlib import Path

_LOOSE = re.compile(r"<Lua_Script>\s*([^<\s][^<]*?)\s*</Lua_Script>", re.I)


def _stem(value: str) -> str:
    return Path(value.replace("\\", "/")).stem.lower()


def references(xml_roots: list[Path], modes: dict) -> dict[str, dict]:
    """script stem -> {'tags': set, 'galactic': bool, 'files': int} over every XML file of the given layer roots."""
    galactic_tags = {t.lower() for t in modes.get("galactic_tags", [])}
    child_tags = {t.lower() for t in modes.get("galactic_child_tags", [])}
    found: dict[str, dict] = {}

    def note(stem: str, tag: str, galactic: bool) -> None:
        row = found.setdefault(stem, {"tags": set(), "galactic": False, "files": 0})
        row["tags"].add(tag)
        row["galactic"] = row["galactic"] or galactic or tag.lower() in galactic_tags
        row["files"] += 1

    for root in xml_roots:
        for directory, _subdirs, files in os.walk(root):
            for name in files:
                if not name.lower().endswith(".xml"):
                    continue
                path = Path(directory) / name
                try:
                    tree = ET.parse(path)
                except (ET.ParseError, OSError, UnicodeDecodeError, ValueError):
                    try:
                        text = path.read_text(encoding="cp1252", errors="replace")
                    except OSError:
                        continue
                    for match in _LOOSE.finditer(text):
                        note(_stem(match.group(1)), "?", False)
                    continue
                for element in tree.iter():
                    script = None
                    galactic = False
                    for child in element:
                        low = child.tag.lower() if isinstance(child.tag, str) else ""
                        if low == "lua_script" and child.text and child.text.strip():
                            script = child.text.strip()
                        elif low in child_tags and (child.text or "").strip():
                            galactic = True
                    if script:
                        note(_stem(script), element.tag, galactic)
    return found


def xml_roots_of(layer_roots: dict[str, Path], layers: list[str]) -> list[Path]:
    out = []
    for layer in layers:
        root = layer_roots.get(layer)
        if root is None:
            continue
        for child in root.iterdir() if root.is_dir() else []:
            if child.is_dir() and child.name.lower() == "data":
                for sub in child.iterdir():
                    if sub.is_dir() and sub.name.lower() == "xml":
                        out.append(sub)
                if layer == "stock":
                    pass
        if layer == "stock":
            direct = [p for p in root.glob("*") if p.is_dir() and p.name.lower() == "xml"]
            out.extend(direct)
    return out


def _xml_files(xml_roots: list[Path]):
    for root in xml_roots:
        for directory, _subdirs, files in os.walk(root):
            for name in files:
                if name.lower().endswith(".xml"):
                    yield Path(directory) / name


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError:
        return ""


_STORY_NAME = re.compile(r"<\s*([A-Za-z_]*Story_Name)\s*>\s*([^<\s][^<]*?)\s*</", re.I)


def galactic_story_scripts(xml_roots: list[Path]) -> set[str]:
    """Stems of the Lua_Script entries of the plot files that campaigns name (`*_Story_Name`): the galactic story scripts.

    Plot files named only by a mission map are tactical and are not included."""
    files = list(_xml_files(xml_roots))
    plot_names: set[str] = set()
    for path in files:
        for match in _STORY_NAME.finditer(_read_text(path)):
            plot_names.add(Path(match.group(2).replace("\\", "/")).name.lower())
    stems: set[str] = set()
    for path in files:
        if path.name.lower() in plot_names:
            stems.update(_stem(m.group(1)) for m in _LOOSE.finditer(_read_text(path)))
    return stems
