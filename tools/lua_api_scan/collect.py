"""Collect the effective Lua scripts of a mod profile from the layer roots (no hard-coded mod paths).

Layer ids come from the shared profile list (docs/tag-coverage/coverage-profiles.json) or from the scan config;
the directory of each layer comes from a local roots mapping (layer id -> directory), or is found by id under a
workshop root. Stock scripts are the base of every mod profile (a mod only replaces the files it ships).
"""
from __future__ import annotations

import dataclasses
import json
import os
import re
from pathlib import Path


class CollectError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class ScriptFile:
    logical: str  # lower-case 'data/scripts/...' path
    layer: str  # layer id the winning bytes come from
    path: Path

    def read(self) -> bytes:
        return self.path.read_bytes()


@dataclasses.dataclass
class Profile:
    id: str
    title: str
    layers_leaf_first: list[str]
    owner_stack: bool
    scripts: dict[str, ScriptFile]  # logical path -> winning file
    missing_layers: list[str]
    meg_lua_unscanned: int = 0


def load_profiles(repo: Path, config: dict) -> list[dict]:
    shared = json.loads((repo / config["profiles_source"]).read_text(encoding="utf-8"))
    layers = {p["id"]: p["layers_leaf_first"] for p in shared["profiles"]}
    out = []
    for row in config["profiles"]:
        layer_list = row.get("layers_leaf_first") or layers.get(row["id"])
        if layer_list is None:
            raise CollectError(f"profile {row['id']}: no layers in the config or the shared profile list")
        out.append({"id": row["id"], "title": row.get("title", row["id"]), "layers_leaf_first": list(layer_list),
                    "owner_stack": bool(row.get("owner_stack"))})
    return out


def _safe_root(raw: str, ident: str) -> Path:
    root = Path(raw).resolve()
    if root == Path(root.anchor) or len(root.parts) < 3:
        raise CollectError(f"{ident}: refusing a drive-root or very shallow directory as a layer root")
    if not root.is_dir():
        raise CollectError(f"{ident}: layer root is not a directory")
    return root


def resolve_roots(roots_file: Path | None, workshop_root: Path | None, stock_root: Path | None,
                  layer_ids: set[str]) -> dict[str, Path]:
    roots: dict[str, Path] = {}
    if roots_file:
        for ident, raw in json.loads(roots_file.read_text(encoding="utf-8")).items():
            if not ident.startswith("_"):
                roots[ident] = _safe_root(raw, ident)
    if stock_root and "stock" not in roots:
        roots["stock"] = _safe_root(str(stock_root), "stock")
    if workshop_root:
        for ident in layer_ids:
            if ident not in roots and ident != "stock" and (workshop_root / ident).is_dir():
                roots[ident] = _safe_root(str(workshop_root / ident), ident)
    return roots


def _walk_scripts(root: Path, stock: bool) -> dict[str, Path]:
    """lower-case logical path -> file, for every .lua under the layer's script folder (case-insensitive)."""
    found: dict[str, Path] = {}
    data = None
    base = root if stock else None
    # The layer's Data folder may be spelled in any case.
    if stock:
        data = next((p for p in root.iterdir() if p.is_dir() and p.name.lower() == "data"), root)
    else:
        data = next((p for p in root.iterdir() if p.is_dir() and p.name.lower() == "data"), None)
    if data is None:
        return found
    for directory, subdirs, files in os.walk(data, followlinks=False):
        rel_dir = Path(directory).relative_to(data).as_posix().lower()
        if not (rel_dir == "scripts" or rel_dir.startswith("scripts/")):
            subdirs[:] = [d for d in subdirs if d.lower() == "scripts"] if rel_dir == "." else []
            continue
        for name in files:
            if name.lower().endswith(".lua"):
                logical = f"data/{rel_dir}/{name.lower()}"
                path = Path(directory) / name
                if path.resolve().is_relative_to(root):
                    found.setdefault(logical, path)
    return found


class LayerCache:
    def __init__(self, roots: dict[str, Path]):
        self.roots = roots
        self._scripts: dict[str, dict[str, Path]] = {}

    def scripts(self, layer: str) -> dict[str, Path]:
        if layer not in self._scripts:
            self._scripts[layer] = _walk_scripts(self.roots[layer], layer == "stock") if layer in self.roots else {}
        return self._scripts[layer]


def build_profile(row: dict, cache: LayerCache) -> Profile:
    missing = [layer for layer in row["layers_leaf_first"] if layer not in cache.roots]
    scripts: dict[str, ScriptFile] = {}
    order = [layer for layer in row["layers_leaf_first"] if layer != "stock"]
    if "stock" in cache.roots:
        order.append("stock")
    for layer in order:
        if layer not in cache.roots:
            continue
        for logical, path in cache.scripts(layer).items():
            scripts.setdefault(logical, ScriptFile(logical, layer, path))
    return Profile(row["id"], row["title"], row["layers_leaf_first"], row["owner_stack"], scripts, missing)


_NAME = re.compile(r'"name"\s*:\s*"([^"]+)"')


def layer_title(root: Path | None) -> str | None:
    """The mod's display name from its modinfo file, when present."""
    if root is None:
        return None
    for path in root.glob("*modinfo*.json"):
        try:
            match = _NAME.search(path.read_text(encoding="utf-8-sig", errors="replace"))
        except OSError:
            continue
        if match:
            return match.group(1)
    return None
