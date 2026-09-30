"""Tests for the observed Windows prototype repeat helper.

Three layers, none of which launches Godot or touches a game install:

* synthetic process observation: a scripted fake process table/clock drives
  ``observe_launch`` and ``assess_observation`` (runs on any platform);
* argument-array policy (any platform);
* Windows only: a harmless Python process tree that loads a renamed copy of
  ``python3.dll`` as the "extension" and writes a PNG, observed with the real
  ctypes probe, and full ``dry-run``/``run`` CLI invocations against a
  synthetic git checkout, synthetic prior evidence and that fake engine.
"""

import contextlib
import hashlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOL_DIRECTORY = ROOT / "tools/validation/p1_capture"
TEST_DIRECTORY = pathlib.Path(__file__).resolve().parent
for directory in (TOOL_DIRECTORY, TEST_DIRECTORY):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))

import win_process_observer as observer  # noqa: E402
import windows_prototype_capture as capture  # noqa: E402

WINDOWS = sys.platform == "win32"
DLL_NAME = "libeawr_godot.windows.template_release.x86_64.dll"
DEVICE_LINE = ("OpenGL API 3.3.0 Core Profile Context 26.9.1.260826 - Compatibility - "
               "Using Device: ATI Technologies Inc. - AMD Radeon RX 7900 XTX")


def sha256_file(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


# ------------------------------------------------------- synthetic observation

LAUNCHER = "C:/fake/engine/launcher_console.exe"
ENGINE = "C:/fake/engine/engine.exe"
EXTENSION = "C:/fake/evidence/project/bin/" + DLL_NAME
EXTENSION_SHA = "a" * 64
EXTENSION_BASE = 0x7FF8_1000_0000
EXTENSION_IMAGE_SIZE = 0x0004_2000
ARGV = [LAUNCHER, "--rendering-method", "gl_compatibility", "--path", "C:/fake/evidence/project", "--",
        "--eawr-capture", "C:/fake/evidence/p0-1/frame.png"]
SEP = "\x1f"
SECOND = 1_000_000_000
FILETIME_BASE = 134_000_000_000_000_000


class FakeClock:
    def __init__(self):
        self.t = 0

    def mono(self):
        self.t += 1000  # every reading advances 1 us, so ordering is strict
        return self.t

    def utc(self):
        return 1_790_000_000 * SECOND + self.t

    def sleep(self, seconds):
        self.t += int(seconds * SECOND)


class FakeHandle:
    def __init__(self, world, pid):
        self.world, self.pid = world, pid
        self.row = world.row(pid)

    def times(self):
        return self.row["creation"], (self.row["creation"] + 1) if not self.world.alive(self.row) else 0

    def has_exited(self):
        return not self.world.alive(self.row)

    def wait(self, _timeout_seconds):
        return self.has_exited()

    def exit_code(self):
        return 0 if self.has_exited() else observer.STILL_ACTIVE

    def terminate(self, code=1):
        if self.pid not in self.world.unkillable:
            self.row["killed"] = True
        return True

    def close(self):
        pass


class FakeWorld:
    """A scripted process table.  Times are seconds on the fake clock."""

    def __init__(self, clock, *, extension_path=EXTENSION, extension_from=0.3, png_at=2.0, root_end=5.0,
                 engine_end=None, engine_restart_at=None, second_holder=False, engine_session=1, module_error=False,
                 engine_ppid=100, engine_argv=None, reused_pid_child=False, image_error=False, image_moved_at=None,
                 helper_child=False, unkillable=(), deny_terminate=False, reuse_on_terminate_open=None, table_fails_from=None,
                 stale_parent_replacement=False):
        self.clock, self.png_at = clock, png_at
        self.image_error, self.image_moved_at = image_error, image_moved_at
        self.unkillable, self.deny_terminate = set(unkillable), deny_terminate
        self.reuse_on_terminate_open, self.table_fails_from = dict(reuse_on_terminate_open or {}), table_fails_from
        root = {"pid": 100, "ppid": 4, "image": LAUNCHER, "start": 0.0, "end": root_end, "session": 1,
                "command_line": SEP.join(ARGV), "modules": lambda t: ["C:/Windows/System32/ntdll.dll"]}
        engine_command = SEP.join([ENGINE] + (engine_argv if engine_argv is not None else ARGV[1:]))

        def engine_modules(t, path=extension_path):
            loaded = ["C:/Windows/System32/ntdll.dll", ENGINE]
            return loaded + ([path] if extension_from is not None and t >= extension_from else [])

        engine_end = engine_end if engine_end is not None else (
            root_end - 0.1 if engine_restart_at is None else engine_restart_at)
        engine = {"pid": 200, "ppid": engine_ppid, "image": ENGINE, "start": 0.1, "end": engine_end,
                  "session": engine_session, "command_line": engine_command, "modules": engine_modules}
        self.rows = [root, engine]
        if engine_restart_at is not None:
            self.rows.append({"pid": 200, "ppid": 100, "image": ENGINE, "start": engine_restart_at,
                              "end": root_end - 0.1, "session": 1, "command_line": engine_command,
                              "modules": lambda t: ["C:/Windows/System32/ntdll.dll", ENGINE, extension_path]})
        if second_holder:
            self.rows.append({"pid": 300, "ppid": 200, "image": ENGINE, "start": 0.2, "end": root_end - 0.2,
                              "session": 1, "command_line": engine_command,
                              "modules": lambda t: [extension_path]})
        if helper_child:
            # A long-lived engine child that never loads the extension.
            self.rows.append({"pid": 300, "ppid": 200, "image": ENGINE, "start": 0.4, "end": 999.0,
                              "session": 1, "command_line": engine_command, "modules": lambda t: [ENGINE]})
        if reused_pid_child:
            # Claims root as parent but was created before it: a reused PID.
            self.rows.append({"pid": 400, "ppid": 100, "image": ENGINE, "start": -1.0, "end": 99.0,
                              "session": 1, "command_line": "", "modules": lambda t: [extension_path]})
        if stale_parent_replacement:
            # The process table still names an owned engine child at PID 500, but
            # that child has exited and an unrelated process, created after the
            # engine and parented elsewhere, now holds the PID.
            self.rows.append({"pid": 500, "ppid": 4, "table_ppid": 200, "image": ENGINE, "start": 0.5, "end": 999.0,
                              "session": 1, "command_line": "", "modules": lambda t: [extension_path],
                              "replacement": True})
        for row in self.rows:
            row["creation"] = FILETIME_BASE + int((row["start"] + 10) * 10_000_000)
            row["killed"] = False
        self.module_error = module_error

    def now(self):
        return self.clock.t / SECOND

    def alive(self, row):
        return row["start"] <= self.now() < row["end"] and not row["killed"]

    def row(self, pid):
        rows = [row for row in self.rows if row["pid"] == pid and self.alive(row)]
        return rows[0] if rows else None

    def png_exists(self, _path):
        return self.png_at is not None and self.now() >= self.png_at

    def module_entry(self, index, path):
        entry = {"path": path, "base_address": 0x7FF8_0000_0000 + index * 0x10_0000, "image_size": 0x1000,
                 "image_error": None}
        if path.endswith(DLL_NAME):
            moved = self.image_moved_at is not None and self.now() >= self.image_moved_at
            entry["base_address"] = EXTENSION_BASE + (0x100_0000 if moved else 0)
            entry["image_size"] = None if self.image_error else EXTENSION_IMAGE_SIZE
            entry["image_error"] = "GetModuleInformation failed (299)" if self.image_error else None
        return entry

    # --- probe interface
    def process_table(self):
        if self.table_fails_from is not None and self.now() >= self.table_fails_from:
            raise OSError(5, "CreateToolhelp32Snapshot failed")
        return {row["pid"]: {"pid": row["pid"], "ppid": row.get("table_ppid", row["ppid"]),
                             "exe_name": os.path.basename(row["image"])}
                for row in self.rows if self.alive(row)}

    def identity(self, pid):
        row = self.row(pid)
        if row is None:
            return None
        return {"pid": pid, "parent_pid": row["ppid"], "image_path": row["image"], "creation_filetime": row["creation"],
                "creation_utc": observer.filetime_to_utc(row["creation"]), "exit_filetime": None,
                "session_id": row["session"], "command_line": row["command_line"], "alive": True}

    def modules(self, pid):
        row = self.row(pid)
        if row is None:
            return None, "gone"
        if self.module_error and row["image"] == ENGINE:
            return None, "EnumProcessModulesEx failed (299)"
        return [self.module_entry(index, path) for index, path in enumerate(row["modules"](self.now()))], None

    def visible_windows(self, pids):
        return {pid: 0 for pid in pids}

    def open(self, pid):
        return FakeHandle(self, pid) if self.row(pid) else None

    def open_for_termination(self, pid):
        if self.deny_terminate:
            return None
        original = self.row(pid)
        if original is not None and self.now() >= self.reuse_on_terminate_open.get(pid, float("inf")):
            # The observed instance exits and an unrelated process takes its PID
            # between the termination snapshot and OpenProcess.
            original["end"] = self.now()
            self.rows.append(dict(original, start=self.now(), end=999.0, killed=False, ppid=4,
                                  creation=original["creation"] + 777, reused=True))
        return self.open(pid)


class FakeProcess:
    def __init__(self, world):
        self.world, self.pid = world, 100
        self.killed = False

    def poll(self):
        root = self.world.rows[0]
        return None if self.world.alive(root) else 0

    def kill(self):
        self.world.rows[0]["killed"] = True

    def wait(self, timeout=None):
        return self.poll()


def expected(**changes):
    value = {"root_pid": 100, "root_image": LAUNCHER, "engine_image": ENGINE, "extension_path": EXTENSION,
             "extension_sha256": EXTENSION_SHA, "session_id": 1, "root_command_line": SEP.join(ARGV),
             "argv_tail": ARGV[1:]}
    value.update(changes)
    return value


def observe_world(world_options=None, *, hash_value=EXTENSION_SHA, image_size_of=None, timeout=30.0, **expect):
    clock = FakeClock()
    world = FakeWorld(clock, **(world_options or {}))
    record = observer.observe_launch(FakeProcess(world), world, "frame.png", DLL_NAME, interval_seconds=0.05,
                                     timeout_seconds=timeout, exit_grace_seconds=1.0, clock=clock,
                                     png_exists=world.png_exists, hash_file=lambda _path: hash_value,
                                     image_size_of=image_size_of or (lambda _path: EXTENSION_IMAGE_SIZE))
    json.dumps(record)  # the record must be serialisable as written
    assessment = observer.assess_observation(record, expected(**expect), split=lambda text: text.split(SEP))
    return record, assessment, world


def observe(world_options=None, **options):
    record, assessment, _ = observe_world(world_options, **options)
    return record, assessment


def rows_for(world, pid):
    return [row for row in world.rows if row["pid"] == pid]


def termination_results(record):
    return {(entry["pid"], entry.get("creation_filetime")): entry["result"]
            for entry in record["termination"]["entries"]}

__all__ = [name for name in globals() if not name.startswith('__')]
