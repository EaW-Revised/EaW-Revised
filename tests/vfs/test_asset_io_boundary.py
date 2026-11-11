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
    # Activation cache uses an injected dedicated player user-data directory and digest-only names;
    # it never resolves effect/include paths or opens mounted assets. Payloads are owned and validated.
    "src/presentation/shader_translation_cache.cpp": "bounded activation cache in dedicated player user data, no asset reads",
    "include/eawr/presentation/shader_translation_cache.hpp": "injected player user-data cache adapter API, no asset reads",
    # Identity-checked native DLL loader; no game-asset or VFS reads.
    "include/eawr/presentation/shader_runtime.hpp": "identity-checked native DLL loader, no asset/VFS reads",
    "src/presentation/shader_runtime.cpp": "identity-checked native DLL loader, no asset/VFS reads",
    # Asks the OS whether a drive letter's volume is a spinning disk (a storage-property query on the volume device, zero
    # bytes read, no file of the game); the model prefetch uses it to size its reader pool.
    "src/core/host_probe.cpp": "volume seek-penalty query for the reader count, no asset read",
    # An opt-in diagnostic trace file written by the galactic AI barrier; it reads no asset.
    "src/campaign/game_session.cpp": "opt-in oracle trace output, no asset read",
}


# Write-only diagnostic sidecars (#5207 GC memory boundary): may create their output folder and write one file,
# never read. Checked line by line, so a later read here fails like any other asset I/O.
DIAGNOSTIC_WRITERS = {"include/eawr/diagnostics/gc_memory_boundary.hpp"}
DIAGNOSTIC_READ = re.compile(
    r"\b(?:fopen|CreateFile[AW]?|ReadFile|std::ifstream|std::fstream|std::wifstream|"
    r"std::filesystem::(?:directory_iterator|recursive_directory_iterator|file_size|exists))\b"
)
DIAGNOSTIC_MUST_FAIL = ["std::ifstream input(path);", "std::fstream file(path);",
                        "for (auto& entry : std::filesystem::directory_iterator(root)) {}"]
DIAGNOSTIC_MUST_PASS = ["std::ofstream boundary(path, std::ios::binary | std::ios::trunc);",
                        "std::filesystem::create_directories(path.parent_path(), error);"]


def diagnostic_violations(relative: str, text: str) -> list[str]:
    return [f"{relative}:{number}: diagnostic writer reads: {line.strip()}"
            for number, line in enumerate(text.splitlines(), 1) if DIAGNOSTIC_READ.search(line.split("//", 1)[0])]


def diagnostic_rule_self_check() -> list[str]:
    errors = [f"diagnostic writer self-check: not caught: {line}"
              for line in DIAGNOSTIC_MUST_FAIL if not diagnostic_violations("probe.hpp", line)]
    for line in DIAGNOSTIC_MUST_PASS:
        errors.extend(diagnostic_violations("probe.hpp", line))
    return errors


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
# The content digest hashes mod files only through platform::read_native_chunks. Its header holds the worker-thread
# machinery; thread_priority.cpp is the one platform call (below-normal priority for that worker).
MODS_DIGEST_THREAD_HEADERS = {"condition_variable", "deque", "mutex", "thread"}
MODS_PRIORITY_HEADERS = {"windows.h", "sys/resource.h", "sys/syscall.h", "unistd.h"}
MODS_TEXT_READER_STREAMS = {"ifstream", "istreambuf_iterator"}

# The digest's own cache files in the user-data cache folder: create the folder, replace atomically through the
# `temporary` sibling, delete that sibling or a stale `partial_path`. Exact identifiers, no other argument.
_DIGEST_CACHE_CALL = re.compile(
    r"\bfs::(?:create_directories\(\s*target\.parent_path\(\)\s*,|rename\(\s*temporary\s*,\s*target\s*,|"
    r"remove\(\s*(?:temporary|partial_path)\s*,)")
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
    index_builder = relative == "src/mods/file_index_build.cpp"
    digest = relative == "src/mods/content_digest.cpp"
    digest_header = relative == "include/eawr/mods/content_digest.hpp"
    priority = relative == "src/mods/thread_priority.cpp"
    index_model = relative in {"src/mods/file_index.cpp", "include/eawr/mods/file_index.hpp"}
    profile_fs = {"canonical", "create_directories", "create_directory", "is_symlink", "rename", "remove"}
    violations: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        if digest:
            # Only the named cache-path forms; fs::remove(path) / fs::rename(path, x) on anything else stays caught.
            code = _DIGEST_CACHE_CALL.sub("digest_cache_operation(", code)
        problems: list[str] = []
        for name in _FS_NAME.findall(code):
            if name not in MODS_FS_ALLOWED and not (profile and name in profile_fs):
                problems.append(f"std::filesystem::{name} is not a listing or stat call")
        if _FS_ESCAPE.search(code):
            problems.append("std::filesystem names must stay visible as fs:: or std::filesystem::")
        for kind, header in _INCLUDE.findall(code):
            if kind == '"':
                if not header.startswith(("eawr/mods/", "eawr/core/")) and not (
                        (profile or windows_profile or digest) and header == "profile_io.hpp") and not (
                        index_builder and header == "eawr/vfs/vfs.hpp") and not (
                        digest and header in {"eawr/platform/native_read.hpp", "../../third_party/xxhash/xxhash.h"}) and not (
                        (digest or priority) and header == "thread_priority.hpp"):
                    problems.append(f"include \"{header}\" is outside the mod core and eawr/core")
            elif not (header in MODS_HEADERS_ALLOWED
                      or (reader and header in MODS_TEXT_READER_ALLOWED)
                      or ((profile or digest) and header == "fstream")
                      or (digest_header and header in MODS_DIGEST_THREAD_HEADERS)
                      or (priority and header in MODS_PRIORITY_HEADERS)
                      or (windows_profile and header == "windows.h")
                      or (index_model and header in {"iosfwd", "istream", "ostream"})):
                problems.append(f"include <{header}> is not an allowed header")
        body = _INCLUDE.sub("", code)   # headers are checked above
        for stream in _STREAM.findall(body):
            if not (reader and stream in MODS_TEXT_READER_STREAMS) and not (
                    profile and stream == "ofstream" and re.search(r"\bstd::ofstream\s+output\s*\(\s*temp\s*,", body)) and not (
                    digest and stream == "ofstream" and re.search(r"\bstd::ofstream\s+out\s*\(\s*temporary\s*,", body)) and not (
                    digest and stream == "ifstream" and re.search(r"\bstd::ifstream\s+in\s*\(\s*cache_path\s*,", body)) and not (
                    digest and stream == "istreambuf_iterator" and re.search(r"\(\s*std::istreambuf_iterator<char>\(in\)\s*\)", body)):
                problems.append(f"{stream} is a file stream outside the reviewed reader")
        if digest:
            # Only the reviewed atomic replacement of the user-cache temporary file is allowed.
            remaining = re.sub(r"\bdetail::replace_profile_file\s*\(\s*temporary\s*,\s*target\s*\)", "cache_replace", body)
            if re.search(r"\breplace_profile_file\s*\(", remaining):
                problems.append("atomic replacement is outside the reviewed cache path")
        file_calls = body
        if profile:
            file_calls = re.sub(r"\bfs::(?:rename|remove)\b", "profile_file_operation", file_calls)
        if windows_profile:
            file_calls = file_calls.replace("MoveFileExW", "profile_atomic_replace")
        if index_builder:
            # This adapter receives a mounted Vfs, not a native path or stream.
            file_calls = file_calls.replace("filesystem.open(", "vfs_candidate_read(")
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
    ("src/mods/file_index_build.cpp", "std::ifstream input(mod_root);"),
    ("src/mods/file_index_build.cpp", "std::ofstream output(mod_root);"),
    ("src/mods/file_index_build.cpp", "fs::remove(path);"),
    ("src/mods/file_index.cpp", "std::ifstream input(cache);"),
    ("src/mods/file_index.cpp", "std::ofstream output(cache);"),
    ("src/mods/discovery.cpp", '#include "eawr/vfs/vfs.hpp"'),
    ("src/mods/profile.cpp", "fs::remove_all(p);"),
    ("src/mods/profile.cpp", "std::ifstream in(path);"),
    ("src/mods/profile.cpp", "std::ofstream output(mod_root, std::ios::binary);"),
    ("src/mods/profile.cpp", "std::ofstream output(\"outside-user-data\", std::ios::binary);"),
    ("src/mods/profile.cpp", "DeleteFileW(p);"),
    ("src/mods/profile.cpp", "fopen(a, \"wb\");"),
    ("src/mods/profile.cpp", "#include <windows.h>"),
    ("src/mods/profile.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/discovery.cpp", "#include <windows.h>"),
    # The digest may touch only its cache path: a raw read of mod files, other removals, other headers stay caught.
    ("src/mods/content_digest.cpp", "std::ifstream in(path, std::ios::binary);"),
    ("src/mods/content_digest.cpp", "std::ifstream in(root / file, std::ios::binary);"),
    ("src/mods/content_digest.cpp", "std::ofstream out(mod_file, std::ios::binary);"),
    ("src/mods/content_digest.cpp", "fs::remove_all(path);"),
    ("src/mods/content_digest.cpp", "fs::remove(path);"),
    ("src/mods/content_digest.cpp", "fs::remove(path, error);"),
    ("src/mods/content_digest.cpp", "fs::rename(path, other);"),
    ("src/mods/content_digest.cpp", "fs::rename(temporary, path, error);"),
    ("src/mods/content_digest.cpp", "detail::replace_profile_file(temporary, path);"),
    ("src/mods/content_digest.cpp", "fs::create_directories(mod_root);"),
    ("src/mods/content_digest.cpp", "fs::create_directories(mod_root, error);"),
    ("src/mods/content_digest.cpp", "fs::remove(temporary, error); fs::remove(path, error);"),
    ("src/mods/content_digest.cpp", "fs::last_write_time(path, t);"),
    ("src/mods/content_digest.cpp", "fs::copy_file(a, b);"),
    ("src/mods/content_digest.cpp", "std::remove(name);"),
    ("src/mods/content_digest.cpp", "fopen(name, \"rb\");"),
    ("src/mods/content_digest.cpp", "#include <windows.h>"),
    ("src/mods/content_digest.cpp", "#include <thread>"),
    ("src/mods/content_digest.cpp", "std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());"),
    ("include/eawr/mods/content_digest.hpp", "#include <fstream>"),
    ("include/eawr/mods/content_digest.hpp", "#include <windows.h>"),
    ("src/mods/record.cpp", "#include <thread>"),
    ("src/mods/record.cpp", "#include <mutex>"),
    ("src/mods/record.cpp", "#include <deque>"),
    ("src/mods/record.cpp", "#include <condition_variable>"),
    ("include/eawr/mods/record.hpp", "#include <thread>"),
    ("src/mods/thread_priority.cpp", "std::ifstream in(mod_file);"),
    ("src/mods/thread_priority.cpp", "DeleteFileW(p);"),
    ("src/mods/discovery.cpp", '#include "thread_priority.hpp"'),
    ("src/mods/discovery.cpp", '#include "../../third_party/xxhash/xxhash.h"'),
    ("src/mods/discovery.cpp", '#include "profile_io.hpp"'),
    ("src/mods/discovery.cpp", '#include "eawr/platform/native_read.hpp"'),
    ("src/mods/content_digest.cpp", '#include "eawr/vfs/vfs.hpp"'),
    ("src/mods/discovery.cpp", "#include <sys/resource.h>"),
    ("src/mods/discovery.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/discovery.cpp", '#include "profile_io.hpp"'),
    ("src/mods/profile_windows.cpp", "DeleteFileW(p);"),
    ("src/mods/profile_windows.cpp", "std::ofstream output(mod_root);"),
]
MODS_PATH_MUST_PASS = [
    ("src/mods/file_index_build.cpp", '#include "eawr/vfs/vfs.hpp"'),
    ("src/mods/file_index_build.cpp", "filesystem.read_chunks(candidate, std::span(buffer.get(), chunk_bytes),"),
    ("src/mods/file_index.cpp", "#include <istream>"),
    ("src/mods/file_index.cpp", "#include <ostream>"),
    ("include/eawr/mods/file_index.hpp", "#include <iosfwd>"),
    ("src/mods/profile.cpp", "std::ofstream output(temp, std::ios::binary | std::ios::trunc);"),
    ("src/mods/profile.cpp", '#include "profile_io.hpp"'),
    ("src/mods/profile_windows.cpp", "MoveFileExW(a, b, 0);"),
    ("src/mods/profile_windows.cpp", "#include <windows.h>"),
    ("src/mods/content_digest.cpp", "std::ifstream in(cache_path, std::ios::binary);"),
    ("src/mods/content_digest.cpp", "std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());"),
    ("src/mods/content_digest.cpp", "std::ofstream out(temporary, std::ios::binary | std::ios::trunc);"),
    ("src/mods/content_digest.cpp", "fs::create_directories(target.parent_path(), error);"),
    ("src/mods/content_digest.cpp", "fs::rename(temporary, target, error);"),
    ("src/mods/content_digest.cpp", "detail::replace_profile_file(temporary, target);"),
    ("src/mods/content_digest.cpp", "fs::remove(partial_path, error);"),
    ("src/mods/content_digest.cpp", "#include <fstream>"),
    ("src/mods/content_digest.cpp", '#include "eawr/platform/native_read.hpp"'),
    ("src/mods/content_digest.cpp", '#include "thread_priority.hpp"'),
    ("src/mods/content_digest.cpp", '#include "profile_io.hpp"'),
    ("src/mods/content_digest.cpp", '#include "../../third_party/xxhash/xxhash.h"'),
    ("src/mods/content_digest.cpp", "const fs::directory_entry entry(path, error);"),
    ("include/eawr/mods/content_digest.hpp", "#include <thread>"),
    ("include/eawr/mods/content_digest.hpp", "#include <mutex>"),
    ("include/eawr/mods/content_digest.hpp", "#include <deque>"),
    ("include/eawr/mods/content_digest.hpp", "#include <condition_variable>"),
    ("src/mods/thread_priority.cpp", "#include <windows.h>"),
    ("src/mods/thread_priority.cpp", "#include <sys/syscall.h>"),
    ("src/mods/thread_priority.cpp", "#include <sys/resource.h>"),
    ("src/mods/thread_priority.cpp", "#include <unistd.h>"),
    ("src/mods/thread_priority.cpp", "SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);"),
    ("src/mods/thread_priority.cpp", "setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);"),
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
    violations: list[str] = mods_rule_self_check() + diagnostic_rule_self_check()
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
            if relative in DIAGNOSTIC_WRITERS:
                violations.extend(diagnostic_violations(relative, path.read_text(encoding="utf-8")))
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
