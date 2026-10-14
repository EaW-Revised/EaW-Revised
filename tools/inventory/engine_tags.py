"""Debug-build name recognition, separate from authored tag application coverage."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

try:
    from .common import ascii_fold
except ImportError:
    from common import ascii_fold

CATALOG = Path(__file__).resolve().parents[2] / "docs/tag-coverage/engine-tags.json"

# Inventory uses the pinned schema's canonical object types, including registry
# hints for concrete unit definitions. These owners have descriptive catalog
# names rather than the canonical XML spelling; other owners match without '-'.
TABLE_OBJECT_TYPES = {
    "object-definition": "gameobjecttype",
    "sound-event": "sfxevent",
    "lightning-effect": "lightningtypemanager",
    "movie": "binkmovie",
    "radar-layout": "radarmap",
    "text-crawl": "draw3dtextcrawl",
    "story-event": "storyparser",
}


class EngineTags:
    def __init__(self, value: dict[str, Any]):
        if value.get("schema_version") != 1:
            raise ValueError("unsupported engine-tag catalog")
        self.value = value
        self.tags: dict[str, list[dict[str, str]]] = {}
        seen = set()
        for row in value["rows"]:
            if set(row) != {"tag", "table_kind", "value_type", "evidence"}:
                raise ValueError("invalid engine-tag row fields")
            if any(not isinstance(v, str) or not v for v in row.values()):
                raise ValueError("empty engine-tag row field")
            key = (ascii_fold(row["tag"]), row["table_kind"], row["value_type"])
            if key in seen:
                raise ValueError("duplicate engine-tag row")
            seen.add(key)
            if row["evidence"] not in value["evidence"]:
                raise ValueError("unresolved engine-tag evidence")
            self.tags.setdefault(ascii_fold(row["tag"]), []).append(row)

    @classmethod
    def load(cls, path: Path = CATALOG) -> EngineTags:
        return cls(json.loads(path.read_text(encoding="utf-8")))

    def recognizes(self, profile: str, object_type: str, tag: str) -> bool:
        # Table ownership is required; nested context acceptance and application
        # still need separate evidence (ML-28). Remake uses the FoC profile.
        if profile not in {"foc", "remake"}:
            return False
        return any(TABLE_OBJECT_TYPES.get(row["table_kind"], row["table_kind"].replace("-", ""))
                   == ascii_fold(object_type) for row in self.tags.get(ascii_fold(tag), []))
