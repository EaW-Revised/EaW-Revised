"""Read what each host binds from the C++ binding tables (no build, no Lua)."""
from __future__ import annotations

import dataclasses
from pathlib import Path

from . import cxx


@dataclasses.dataclass
class HostBindings:
    host: str
    globals: dict[str, list[str]]  # global or Namespace.Function -> ["file:line", ...]
    members: dict[str, list[str]]  # method name -> sites
    stubs: set[str]  # names registered as a fault-by-name stub (calling them raises "is unbound")
    infrastructure: set[str]
    problems: list[str]

    def knows_global(self, name: str) -> bool:
        return name in self.globals or name in self.infrastructure

    def knows_member(self, name: str) -> bool:
        return name in self.members


def _read(root: Path, rel: str) -> str:
    return (root / rel).read_text(encoding="utf-8", errors="replace")


def read_host(root: Path, host: dict, follow: dict[str, dict] | None = None) -> HostBindings:
    out = HostBindings(host["id"], {}, {}, set(), set(), [])
    follow = follow or {}
    followed: set[str] = set()
    queue = list(host.get("bindings", []))
    while queue:
        source = queue.pop(0)
        rel = source["file"]
        try:
            text = _read(root, rel)
        except OSError as error:
            out.problems.append(f"{rel}: {error}")
            continue
        try:
            for reg in cxx.read_registrations(text, rel, source["registrars"], source.get("function")):
                if reg.kind == "member" and "." in reg.name:
                    continue  # a query string, never a Lua method name
                site = f"{rel}:{reg.line}"
                (out.globals if reg.kind == "global" else out.members).setdefault(reg.name, []).append(site)
            for listed in source.get("name_lists", []):
                for name in cxx.read_name_list(text, listed["variable"], source.get("function")):
                    if cxx.API_NAME.match(name):
                        out.globals.setdefault(name, []).append(f"{rel}:implemented")
            # A registration helper the host calls binds its names too (config 'follow').
            for callee in sorted(cxx.called_identifiers(text, source.get("function")) & follow.keys() - followed):
                followed.add(callee)
                queue.append(follow[callee])
        except ValueError as error:
            out.problems.append(str(error))
    infra = host.get("infrastructure")
    if infra:
        try:
            out.infrastructure.update(cxx.read_object_specs(_read(root, infra["file"]), infra["function"]))
        except (OSError, ValueError) as error:
            out.problems.append(f"infrastructure: {error}")
    stubs = host.get("stubs")
    if stubs:
        try:
            listed = cxx.read_name_list(_read(root, stubs["file"]), stubs["array"])
            implemented = set()
            if stubs.get("implemented_variable"):
                implemented = set(cxx.read_name_list(_read(root, stubs["implemented_file"]), stubs["implemented_variable"],
                                                      stubs.get("implemented_function")))
            # Wrapper-variable names are marked implemented by a loop in the host; they are not global stubs.
            out.stubs = {name for name in listed if name not in implemented and name not in out.globals}
        except (OSError, ValueError) as error:
            out.problems.append(f"stubs: {error}")
    return out


def read_all(root: Path, hosts: list[dict], follow_rows: list[dict] | None = None) -> dict[str, HostBindings]:
    follow = {row["function"]: {"file": row["file"], "registrars": row["registrars"],
                                **({} if row.get("whole_file") else {"function": row["function"]})}
              for row in (follow_rows or [])}
    return {host["id"]: read_host(root, host, follow) for host in hosts if host.get("bindings") or host.get("infrastructure")}
