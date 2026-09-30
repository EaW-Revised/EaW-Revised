"""Result cache of the compiler-backed boundary checks (check_sim_boundary.py, check_lua_numeric_boundary.py).

A checked translation unit's result is reused only when nothing Clang read for it has changed:
the entry keeps the SHA-256 of every file of the unit's dependency file (the unit, every
project and system header), and the names in every project directory holding one of them and
in every include directory, so a new header that could shadow an included one invalidates it
too. The key covers the checker's own sources, the Clang executable and its version, the
command line and VCToolsInstallDir. Compile errors are never stored. Entries live in the build
tree (one JSON file each); a missing or unreadable entry is a miss.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
import threading
from pathlib import Path
from typing import Any, Iterable

MAX_ENTRIES = 4000
KEEP_ENTRIES = 2000
CACHE_VERSION = 2  # Old entries did not watch intermediate include directories.
_TOKEN = re.compile(r"(?:\\ |[^\s])+")
# Standard library headers have no suffix.
INCLUDABLE_SUFFIXES = {"", ".h", ".hh", ".hpp", ".hxx", ".h++", ".inc", ".inl", ".ipp", ".tpp", ".def",
                       ".c", ".cc", ".cpp", ".cxx"}


def sha256_file(path: Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def parse_depfile(text: str) -> list[str]:
    """The prerequisites of a Make-style dependency file as Clang writes it (escaped spaces, continuations)."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    # The target ends at the first ': ' (a Windows drive letter's colon is followed by a backslash).
    _, separator, rest = text.partition(": ")
    if not separator:
        return []
    return [token.replace("\\ ", " ").replace("$$", "$") for token in _TOKEN.findall(rest)]


class BoundaryCache:
    """Per-unit results keyed by content. directory None disables the cache (every lookup misses)."""

    def __init__(self, directory: Path | None, root: Path, identity: dict[str, Any],
                 include_dirs: Iterable[Path] = (), aliases: dict[Path, str] | None = None):
        self.directory = directory
        self.root = root.resolve()
        self.include_dirs = [Path(d).resolve() for d in include_dirs]
        # Temporary directories (the generated XML schema) are stored under a stable alias.
        self.aliases = {Path(k).resolve(): v for k, v in (aliases or {}).items()}
        self.identity = json.dumps(identity, sort_keys=True)
        self._hashes: dict[str, str | None] = {}
        self._listings: dict[str, str | None] = {}
        self._lock = threading.Lock()
        self.hits = self.misses = 0
        if directory is not None:
            directory.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def clang_identity(clang: str) -> dict[str, str]:
        try:
            version = subprocess.run([clang, "--version"], capture_output=True, text=True, errors="replace",
                                     timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            version = ""
        return {"clang": clang, "clang_version": version,
                "VCToolsInstallDir": os.environ.get("VCToolsInstallDir", "")}

    def _alias(self, path: Path) -> str:
        for real, alias in self.aliases.items():
            try:
                return alias + "/" + path.relative_to(real).as_posix()
            except ValueError:
                continue
        return os.path.normcase(str(path))

    def _unalias(self, name: str) -> Path:
        for real, alias in self.aliases.items():
            if name.startswith(alias + "/"):
                return real / name[len(alias) + 1:]
        return Path(name)

    def _hash(self, name: str) -> str | None:
        with self._lock:
            if name in self._hashes:
                return self._hashes[name]
        value = sha256_file(self._unalias(name))
        with self._lock:
            self._hashes[name] = value
        return value

    def _listing(self, directory: str) -> str | None:
        with self._lock:
            if directory in self._listings:
                return self._listings[directory]
        try:
            # Only what an #include could name: subdirectories and C/C++ sources and headers
            # (not the __pycache__ a test leaves and a build host's git clean removes).
            names = sorted(entry.name for entry in os.scandir(self._unalias(directory))
                           if entry.name != "__pycache__"
                           and (entry.is_dir() or os.path.splitext(entry.name)[1].lower() in INCLUDABLE_SUFFIXES))
            value = hashlib.sha256("\n".join(names).encode()).hexdigest()
        except FileNotFoundError:
            # A missing intermediate include directory can appear later and shadow
            # a header from a lower-priority -I root.
            value = "missing"
        except OSError:
            value = None
        with self._lock:
            self._listings[directory] = value
        return value

    def _entry_path(self, key: dict[str, Any]) -> Path:
        digest = hashlib.sha256((str(CACHE_VERSION) + "\n" + self.identity + "\n" +
                                 json.dumps(key, sort_keys=True)).encode()).hexdigest()
        assert self.directory is not None
        return self.directory / f"{digest}.json"

    def lookup(self, key: dict[str, Any]) -> Any | None:
        """The stored result when every recorded file and directory is unchanged, else None."""
        if self.directory is None:
            return None
        path = self._entry_path(key)
        try:
            entry = json.loads(path.read_text(encoding="utf-8"))
            valid = (all(self._hash(name) == digest for name, digest in entry["files"].items())
                     and all(self._listing(name) == digest for name, digest in entry["directories"].items()))
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            valid = False
        with self._lock:
            if valid:
                self.hits += 1
            else:
                self.misses += 1
        if not valid:
            return None
        try:
            os.utime(path)  # recently used entries survive pruning
        except OSError:
            pass
        return entry["result"]

    def store(self, key: dict[str, Any], depfile: Path, result: Any) -> None:
        if self.directory is None:
            return
        try:
            prerequisites = parse_depfile(depfile.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            return
        if not prerequisites:
            return
        files: dict[str, str] = {}
        directories: set[str] = {self._alias(d) for d in self.include_dirs}
        for item in prerequisites:
            # Resolved, so a short (8.3) Windows temporary path still matches its alias.
            resolved = (Path(item) if os.path.isabs(item) else Path.cwd() / item).resolve()
            name = self._alias(resolved)
            digest = sha256_file(resolved)
            if digest is None:
                return  # a dependency vanished while checking: do not store
            files[name] = digest
            parent = resolved.parent
            if parent == self.root or self.root in parent.parents or name != os.path.normcase(str(resolved)):
                directories.add(self._alias(parent))
            # If a dependency came from a later -I root, a matching relative
            # header in any earlier root would take precedence. Record every
            # directory on that candidate path, including ones absent today.
            for index, include_dir in enumerate(self.include_dirs):
                try:
                    relative = resolved.relative_to(include_dir)
                except ValueError:
                    continue
                for earlier in self.include_dirs[:index]:
                    candidate = earlier
                    directories.add(self._alias(candidate))
                    for part in relative.parts[:-1]:
                        candidate = candidate / part
                        directories.add(self._alias(candidate))
        with self._lock:
            self._hashes.update(files)
        entry = {"files": files,
                 "directories": {name: self._listing(name) for name in sorted(directories)},
                 "result": result}
        if any(value is None for value in entry["directories"].values()):
            return
        path = self._entry_path(key)
        handle, temporary = tempfile.mkstemp(dir=self.directory, suffix=".tmp")
        try:
            with os.fdopen(handle, "w", encoding="utf-8") as stream:
                json.dump(entry, stream)
            os.replace(temporary, path)
        except OSError:
            try:
                os.unlink(temporary)
            except OSError:
                pass

    def prune(self) -> None:
        """Keeps the most recently used entries once the directory grows past MAX_ENTRIES."""
        if self.directory is None:
            return
        try:
            entries = [(entry.stat().st_mtime, entry.path) for entry in os.scandir(self.directory)
                       if entry.name.endswith((".json", ".tmp"))]
        except OSError:
            return
        if len(entries) <= MAX_ENTRIES:
            return
        for _, path in sorted(entries, reverse=True)[KEEP_ENTRIES:]:
            try:
                os.unlink(path)
            except OSError:
                pass

    def summary(self) -> str:
        if self.directory is None:
            return "result cache off"
        return f"result cache: {self.hits} reused, {self.misses} checked"
