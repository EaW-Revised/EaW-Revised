#!/usr/bin/env python3
"""Print a local dump's exception stack using Windows DbgHelp and private PDBs."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import minidump_symbols


def debugger_path(explicit=None):
    if explicit:
        return str(explicit)
    found = shutil.which("cdb.exe") or shutil.which("cdb")
    if found:
        return found
    for variable in ("ProgramFiles(x86)", "ProgramFiles"):
        if base := os.environ.get(variable):
            candidate = Path(base) / "Windows Kits/10/Debuggers/x64/cdb.exe"
            if candidate.is_file():
                return str(candidate)
    raise FileNotFoundError("Install Debugging Tools for Windows, or pass --debugger <cdb.exe>")


def cdb_symbolize(dump, store, debugger=None):
    dump, store = Path(dump).resolve(), Path(store).resolve()
    if not dump.is_file() or not store.is_dir():
        raise ValueError("dump and private symbol store must exist")
    if any(character in str(store) for character in '*;"\r\n'):
        raise ValueError("symbol store path contains debugger control characters")
    # One local symbol server, with no public server or upstream network path.
    environment = dict(os.environ, _NT_SYMBOL_PATH="", _NT_ALT_SYMBOL_PATH="")
    result = subprocess.run([debugger_path(debugger), "-y", "srv*" + str(store),
                             "-z", str(dump), "-c", ".ecxr;kv;q"],
                            capture_output=True, text=True, errors="replace", timeout=120, env=environment)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout or f"debugger exit {result.returncode}")
    return result.stdout + result.stderr


def symbolize(dump, store, debugger=None, llvm=None):
    if debugger:
        return cdb_symbolize(dump, store, debugger)
    if not llvm:
        return minidump_symbols.symbolize(dump, store)
    # LLVM versions provisioned on the pool predate --pdb. Stage each matching
    # pair together so ordinary COFF PDB discovery works without a new install.
    with tempfile.TemporaryDirectory(prefix="eawr-symbolizer-") as temporary:
        staged = {}

        def lookup(module, address):
            if module.image not in staged:
                folder = Path(temporary) / str(len(staged)); folder.mkdir()
                for source in (module.image, module.pdb):
                    try:
                        os.link(source, folder / source.name)
                    except OSError:
                        shutil.copyfile(source, folder / source.name)
                staged[module.image] = folder / module.image.name
            result = subprocess.run([str(llvm), "--relative-address", "--obj=" + str(staged[module.image]),
                                     hex(address - module.base)], capture_output=True,
                                    text=True, errors="replace", timeout=60)
            if result.returncode:
                raise RuntimeError(result.stderr or "llvm-symbolizer failed")
            name = result.stdout.splitlines()[0] if result.stdout.strip() else ""
            return "" if name == "??" else name

        return minidump_symbols.symbolize(dump, store, lookup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--store", type=Path, required=True)
    backend = parser.add_mutually_exclusive_group()
    backend.add_argument("--debugger", type=Path, help="optional CDB backend; Windows DbgHelp is the default")
    backend.add_argument("--llvm-symbolizer", type=Path, help="optional LLVM name lookup with native dump unwinding")
    args = parser.parse_args()
    try:
        print(symbolize(args.dump, args.store, args.debugger, args.llvm_symbolizer), end="")
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"symbolize: {error}\n")


if __name__ == "__main__":
    main()
