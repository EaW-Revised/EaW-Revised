#!/usr/bin/env python3
"""Keep runtime game-asset native I/O inside VFS/platform adapters."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


FORBIDDEN = re.compile(
    r"#\s*include\s*[<\"]filesystem[>\"]|"
    r"#\s*include\s*[<\"]fstream[>\"]|"
    r"\b(?:fopen|CreateFile[AW]?|ReadFile|std::ifstream|std::fstream|std::filesystem)\b"
)


# Reviewed native I/O that is not a game-asset read through the virtual file system.
REVIEWED = {
    # Copies the four interface faces out of the player's own game executable into user data (one file, validated
    # structurally and by pinned hash); the executable is not a mounted asset.
    "src/presentation/ui/font_extract.cpp": "font extraction from the player's own executable into user data",
    "include/eawr/presentation/ui/font_extract.hpp": "font extraction from the player's own executable into user data",
    # An opt-in diagnostic trace file written by the galactic AI barrier; it reads no asset.
    "src/campaign/game_session.cpp": "opt-in oracle trace output, no asset read",
}


# The mod core is a native adapter for listings and Steam metadata. The profile format is the sole
# writer, with an API restricted to a caller-supplied user-data folder plus validated profile id.
# Its scoped exceptions below never grant asset reads or permit other mod-core files to write.
MODS_PREFIXES = ("src/mods/", "include/eawr/mods/")
MODS_FS_ALLOWED = {
    "path", "directory_iterator", "directory_options", "directory_entry", "file_type", "file_time_type",
    "filesystem_error", "is_directory", "is_regular_file", "exists", "file_size", "status", "symlink_status",
    "absolute", "weakly_canonical", "equivalent",
}
MODS_HEADERS_ALLOWED = {
    "algorithm", "array", "atomic", "charconv", "chrono", "cmath", "cstddef", "cstdint", "cstdio", "cstdlib",
    "cstring", "exception", "filesystem", "functional", "iterator", "limits", "map", "memory", "optional", "set",
    "span", "string", "string_view", "system_error", "tuple", "type_traits", "unordered_map", "unordered_set",
    "utility", "variant", "vector",
}
# The one reviewed reader: Steam's libraryfolders.vdf and appworkshop_*.acf, opened read-only.
MODS_TEXT_READER = "src/mods/steam/library_folders.cpp"
MODS_TEXT_READER_ALLOWED = {"fstream"}            # header
MODS_TEXT_READER_STREAMS = {"ifstream", "istreambuf_iterator"}

_FS_NAME = re.compile(r"\b(?:std::)?(?:filesystem|fs)::(\w+)")
_FS_ESCAPE = re.compile(
    r"\busing\s+namespace\s+(?:std::)?filesystem\b|\busing\s+(?:std::)?filesystem::|"
    r"\bnamespace\s+(?!fs\b)\w+\s*=\s*(?:\w+::)*(?:filesystem|fs)\s*;"
)
_INCLUDE = re.compile(r"#\s*include\s*([<\"])([^>\"]+)[>\"]")
_STREAM = re.compile(r"\b(?:std::)?(\w*(?:fstream|filebuf|streambuf_iterator))\b")
_C_FILE_CALL = re.compile(
    r"\b(?:std::)?(?:_?w?fopen(?:_s)?|_?w?freopen(?:_s)?|_w?f?open|_w?creat|_?w?remove|_?w?unlink|_?w?rename|"
    r"_?w?mkdir|_?w?rmdir|_?w?chmod|_?w?utime\w*|tmpfile\w*|tmpnam\w*|fwrite|fputs|fprintf|ftruncate|truncate|"
    r"open|creat)\s*\("
)
_WIN32_FILE_CALL = re.compile(
    r"\b(?:CreateFile|CreateDirectory|SetFile\w*|WriteFile|DeleteFile|MoveFile|CopyFile|RemoveDirectory|ReplaceFile|"
    r"SetEndOfFile|CreateHardLink|CreateSymbolicLink|ReadFile|OpenFile)\w*\b"
)


def mods_violations(relative: str, text: str) -> list[str]:
    reader = relative == MODS_TEXT_READER
    # Sole writer: caller-supplied user data, covered by mods_profile_contracts. No asset IO.
    profile = relative == "src/mods/profile.cpp"
    windows_profile = relative == "src/mods/profile_windows.cpp"
    profile_fs = {"canonical", "create_directories", "create_directory", "is_symlink", "rename", "remove"}
    violations: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        problems: list[str] = []
        for name in _FS_NAME.findall(code):
            if name not in MODS_FS_ALLOWED and not (profile and name in profile_fs):
                problems.append(f"std::filesystem::{name} is not a listing or stat call")
        if _FS_ESCAPE.search(code):
            problems.append("std::filesystem names must stay visible as fs:: or std::filesystem::")
        for kind, header in _INCLUDE.findall(code):
            if kind == '"':
                if not header.startswith(("eawr/mods/", "eawr/core/")) and not (
                        (profile or windows_profile) and header == "profile_io.hpp"):
                    problems.append(f"include \"{header}\" is outside the mod core and eawr/core")
            elif not (header in MODS_HEADERS_ALLOWED
                      or (reader and header in MODS_TEXT_READER_ALLOWED)
                      or (profile and header == "fstream")
                      or (windows_profile and header == "windows.h")):
                problems.append(f"include <{header}> is not an allowed header")
        body = _INCLUDE.sub("", code)   # headers are checked above
        for stream in _STREAM.findall(body):
            if not (reader and stream in MODS_TEXT_READER_STREAMS) and not (
                    profile and stream == "ofstream" and re.search(r"\bstd::ofstream\s+output\s*\(\s*temp\s*,", body)):
                problems.append(f"{stream} is a file stream outside the reviewed reader")
        file_calls = body
        if profile:
            file_calls = re.sub(r"\bfs::(?:rename|remove)\b", "profile_file_operation", file_calls)
        if windows_profile:
            file_calls = file_calls.replace("MoveFileExW", "profile_atomic_replace")
        if _C_FILE_CALL.search(file_calls) or _WIN32_FILE_CALL.search(file_calls):
            problems.append("C or Win32 file calls are not used in the mod core")
        for problem in problems:
            violations.append(f"{relative}:{line_number}: mod core: {problem}: {line.strip()}")
    return violations


# Negative fixtures: each line must be caught (outside the reviewed reader), and the allowed lines must pass. They
# run every time, so a loosened rule fails here first.
MODS_MUST_FAIL = [
    "std::ofstream out(path);", "std::wofstream out(path);", "std::basic_ofstream<char> out(path);",
    "std::fstream both(path);", "std::wfstream both(path);", "std::ifstream in(path);", "std::wifstream in(path);",
    "std::filebuf buffer;", "#include <fstream>", "#include <windows.h>", "#include <io.h>", "#include <fcntl.h>",
    "#include <unistd.h>", "#include \"platform/files.hpp\"",
    "FILE* f = fopen(name, \"wb\");", "_wfopen_s(&f, name, L\"wb\");", "fopen_s(&f, name, \"wb\");",
    "std::remove(name);", "_wremove(name);", "std::rename(a, b);", "_unlink(name);", "_wunlink(name);",
    "CreateDirectoryW(name, nullptr);", "SetFileAttributesW(name, 0);", "SetFileTime(handle, a, b, c);",
    "CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);",
    "fs::remove(p);", "std::filesystem::remove(p);", "fs::remove_all(p);", "fs::rename(a, b);", "fs::copy_file(a, b);",
    "fs::create_directories(p);", "fs::permissions(p, fs::perms::none);", "fs::resize_file(p, 0);",
    "fs::last_write_time(p, t);", "fs::last_write_time(entry.path(), t);", "fs::last_write_time(p);",
    "using namespace std::filesystem;", "using std::filesystem::remove;", "namespace io = std::filesystem;",
]
MODS_MUST_PASS = [
    "namespace fs = std::filesystem;", "#include <filesystem>", "#include \"eawr/core/sha256.hpp\"",
    "for (fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;",
    "const fs::file_time_type time = entry.last_write_time(time_error);",
    "const fs::file_type type = entry.symlink_status(status_error).type();",
    "if (!fs::is_regular_file(path, error)) return std::nullopt;",
    "std::vector<std::pair<std::string, std::string>> files; // remove() and rename() in a comment",
]
MODS_READER_MUST_PASS = [
    "#include <fstream>", "std::ifstream stream(path, std::ios::in | std::ios::binary);",
    "return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());",
]
# Path-aware fixtures pin the sole writer to its user-data staging file and keep the private platform
# primitive out of every other module. In particular, allowing ofstream here cannot grant a mod-root write.
MODS_PATH_MUST_FAIL = [
    ("src/mods/profile.cpp", "fs::remove_all(p);"),
    ("src/mods/profile.cpp", "std::ifstream in(path);"),
    ("src/mods/profile.cpp", "std::ofstream output(mod_root, std::ios::binary);"),
    ("src/mods/profile.cpp", "std::ofstream output(\"outside-user-data\", std::ios::binary);"),
    ("src/mods/profile.cpp", "DeleteFileW(p);"),
    ("src/mods/profile.cpp", "fopen(a, \"wb\");"),
    ("src/mods/profile.cpp", "#include <windows.h>"),
    ("src/mods/profile.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/discovery.cpp", "#include <windows.h>"),
    ("src/mods/discovery.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/discovery.cpp", '#include "profile_io.hpp"'),
    ("src/mods/profile_windows.cpp", "DeleteFileW(p);"),
    ("src/mods/profile_windows.cpp", "std::ofstream output(mod_root);"),
]
MODS_PATH_MUST_PASS = [
    ("src/mods/profile.cpp", "std::ofstream output(temp, std::ios::binary | std::ios::trunc);"),
    ("src/mods/profile.cpp", '#include "profile_io.hpp"'),
    ("src/mods/profile_windows.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/profile_windows.cpp", "#include <windows.h>"),
]


def mods_rule_self_check() -> list[str]:
    errors: list[str] = []
    for path, line in MODS_PATH_MUST_FAIL:
        if not mods_violations(path, line):
            errors.append(f"mod core rule self-check: not caught in {path}: {line}")
    for path, line in MODS_PATH_MUST_PASS:
        errors.extend(mods_violations(path, line))
    for line in MODS_MUST_FAIL:
        if not mods_violations("src/mods/probe.cpp", line):
            errors.append(f"mod core rule self-check: not caught: {line}")
    for line in MODS_MUST_PASS:
        for violation in mods_violations("src/mods/probe.cpp", line):
            errors.append(f"mod core rule self-check: allowed line flagged: {violation}")
    for line in MODS_READER_MUST_PASS:
        for violation in mods_violations(MODS_TEXT_READER, line):
            errors.append(f"mod core rule self-check: reader line flagged: {violation}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    arguments = parser.parse_args()
    roots = [arguments.root / "src", arguments.root / "include" / "eawr"]
    violations: list[str] = mods_rule_self_check()
    for source_root in roots:
        for path in source_root.rglob("*"):
            if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".hpp", ".h"}:
                continue
            relative = path.relative_to(arguments.root).as_posix()
            # Platform headers declare the native adapter API just as platform sources implement it.
            if relative.startswith(("src/vfs/", "src/platform/", "include/eawr/platform/", "include/eawr/vfs/")):
                continue
            if relative in REVIEWED:
                continue
            if relative.startswith(MODS_PREFIXES):
                violations.extend(mods_violations(relative, path.read_text(encoding="utf-8")))
                continue
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if FORBIDDEN.search(line):
                    violations.append(f"{relative}:{line_number}: native asset I/O surface: {line.strip()}")
    if violations:
        print("\n".join(violations), file=sys.stderr)
        return 1
    print("asset I/O boundary passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
