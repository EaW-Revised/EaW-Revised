#!/usr/bin/env python3
"""Refresh the FoC space-skirmish roster and its mechanic evidence (UC-01..06)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict, deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
from tools.inventory.corpus import Corpus, CorpusError


def rule_index():
    rules = {}
    tickets = {}
    gaps = defaultdict(list)
    tag_rules = defaultdict(set)
    for path in sorted((ROOT / "docs/behaviour/walks").glob("*.md")):
        # Galactic walks (gc-*.md) describe phase-3 rules: they are indexed for references but are not
        # space-consumption evidence or space-census gaps.
        galactic = path.name.startswith("gc-")
        context = []
        for line in path.read_text(encoding="utf-8").splitlines():
            ids = re.findall(r"\bW[A-Z]{2}-\d+\b", line)
            if line.startswith("#"):
                context = []
            if re.match(r"(?:\| W[A-Z]{2}-|[-*] \*\*W[A-Z]{2}-)", line):
                context = ids[:1]
            for tag in re.findall(r"`([A-Za-z][A-Za-z0-9_/]*)`", line):
                if "_" in tag and not galactic:
                    tag_rules[tag.lower().split("/")[-1]].update(ids or context)
            for rule in ids:
                rules[rule] = path.relative_to(ROOT).as_posix()
            if line.startswith("| G") and not galactic:
                for ticket in re.findall(r"legacy #(\d+)", line):
                    gaps[int(ticket)].append({"source": path.relative_to(ROOT).as_posix(), "text": line})
            for label, number in re.findall(r"([A-Za-z][A-Za-z -]+) \(legacy #(\d+)\)", line):
                tickets.setdefault(int(number), label.strip())
    return rules, tickets, gaps, tag_rules


def region_digest(source, region):
    """Hash one reviewed LF-normalized region bounded by unique literal anchors.

    Include the start anchor and exclude the end anchor. Missing, reordered or
    duplicate anchors invalidate the review rather than guessing a new boundary.
    """
    source = source.replace("\r\n", "\n")
    start, end = region["start"], region["end"]
    if not start or not end or source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("reviewed region anchors must each occur exactly once")
    first, last = source.index(start), source.index(end)
    if last <= first:
        raise ValueError("reviewed region anchors are out of order")
    return hashlib.sha256(source[first:last].encode("utf-8")).hexdigest()


def changed_fingerprints(review, root=ROOT):
    changed = []
    for name, region in sorted(review["regions"].items()):
        try:
            digest = region_digest((root / region["path"]).read_text(encoding="utf-8"), region)
        except (OSError, ValueError) as exc:
            changed.append(name + " (" + str(exc) + ")")
            continue
        if digest != region["sha256_lf"]:
            changed.append(name)
    return changed
