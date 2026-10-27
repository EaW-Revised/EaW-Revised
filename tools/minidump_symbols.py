"""Offline AMD64 minidump unwinding and name lookup using Windows DbgHelp.

Layouts are the public Windows SDK MINIDUMP, STACKFRAME64 and SYMBOL_INFOW
contracts. The memory callback reads dump ranges and matching retained PE images;
it never reads the analyst process as though it were the crashed process.
"""
from __future__ import annotations
import ctypes as c
from bisect import bisect_right
from dataclasses import dataclass
import os
from pathlib import Path
import struct

import symbol_store

U32, U64, PTR = c.c_uint32, c.c_uint64, c.c_void_p


class Address(c.Structure):
    _fields_ = [("Offset", U64), ("Segment", c.c_uint16), ("Mode", U32)]


class KernelHelp(c.Structure):
    _fields_ = [("Thread", U64), ("ThCallbackStack", U32), ("ThCallbackBStore", U32),
                ("NextCallback", U32), ("FramePointer", U32),
                ("KiCallUserMode", U64), ("KeUserCallbackDispatcher", U64),
                ("SystemRangeStart", U64), ("KiUserExceptionDispatcher", U64),
                ("StackBase", U64), ("StackLimit", U64), ("BuildVersion", U32),
                ("RetpolineStubFunctionTableSize", U32), ("RetpolineStubFunctionTable", U64),
                ("RetpolineStubOffset", U32), ("RetpolineStubSize", U32), ("Reserved0", U64 * 2)]


class StackFrame(c.Structure):
    _fields_ = [(name, Address) for name in ("AddrPC", "AddrReturn", "AddrFrame", "AddrStack", "AddrBStore")]
    _fields_ += [("FuncTableEntry", PTR), ("Params", U64 * 4), ("Far", U32),
                 ("Virtual", U32), ("Reserved", U64 * 3), ("KdHelp", KernelHelp)]


class SymbolInfo(c.Structure):
    _fields_ = [("SizeOfStruct", U32), ("TypeIndex", U32), ("Reserved", U64 * 2),
                ("Index", U32), ("Size", U32), ("ModBase", U64), ("Flags", U32),
                ("Value", U64), ("Address", U64), ("Register", U32), ("Scope", U32),
                ("Tag", U32), ("NameLen", U32), ("MaxNameLen", U32), ("Name", c.c_uint16 * 1)]


@dataclass
class Module:
    name: str
    base: int
    size: int
    timestamp: int
    pdb_name: str = ""
    pdb_key: str = ""
    image: Path | None = None
    pdb: Path | None = None
    image_bytes: bytes = b""


class Dump:
    def __init__(self, data):
        self.data = data
        self.function_tables = {}
        if data[:4] != b"MDMP":
            raise ValueError("not a minidump")
        count, directory = self.unpack(8, "<II")
        self.streams = {}
        for index in range(count):
            kind, size, offset = self.unpack(directory + index * 12, "<III")
            self.slice(offset, size)
            self.streams[kind] = (offset, size)
        if 4 not in self.streams or 6 not in self.streams or 7 not in self.streams:
            raise ValueError("dump needs module, exception and system information streams")
        architecture, = self.unpack(self.streams[7][0], "<H")
        if architecture != 9:
            raise ValueError("only the viewer's AMD64 dump format is supported")
        exception = self.streams[6][0]
        self.thread_id, = self.unpack(exception, "<I")
        self.code, = self.unpack(exception + 8, "<I")
        self.address, = self.unpack(exception + 24, "<Q")
        size, offset = self.unpack(exception + 160, "<II")
        self.context = self.slice(offset, size)
        if len(self.context) < 1232:
            raise ValueError("truncated AMD64 exception context")
        self.modules = []
        offset = self.streams[4][0]
        count, = self.unpack(offset, "<I")
        for index in range(count):
            start = offset + 4 + index * 108
            self.slice(start, 108)
            base, size, _, timestamp, name_rva = self.unpack(start, "<QIIII")
            length, = self.unpack(name_rva, "<I")
            name = self.slice(name_rva + 4, length).decode("utf-16-le").replace("\\", "/").rsplit("/", 1)[-1]
            if not name or name in (".", "..") or any(char in name for char in '/\\:*?"<>|'):
                raise ValueError("invalid dump module filename")
            cv_size, cv_rva = self.unpack(start + 76, "<II")
            module = Module(name, base, size, timestamp)
            if cv_size:
                record = self.slice(cv_rva, cv_size)
                if record[:4] == b"RSDS":
                    module.pdb_name, module.pdb_key = symbol_store.codeview(record)
            self.modules.append(module)
        self.ranges = []
        if 5 in self.streams:
            start = self.streams[5][0]
            count, = self.unpack(start, "<I")
            for index in range(count):
                address, size, offset = self.unpack(start + 4 + index * 16, "<QII")
                self.slice(offset, size)
                self.ranges.append((address, size, offset))
        if 9 in self.streams:
            start = self.streams[9][0]
            count, offset = self.unpack(start, "<QQ")
            for index in range(count):
                address, size = self.unpack(start + 16 + index * 16, "<QQ")
                self.slice(offset, size)
                self.ranges.append((address, size, offset)); offset += size
        if 3 in self.streams:
            start = self.streams[3][0]
            count, = self.unpack(start, "<I")
            for index in range(count):
                address, size, offset = self.unpack(start + 4 + index * 48 + 24, "<QII")
                self.slice(offset, size)
                self.ranges.append((address, size, offset))

    def slice(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("truncated minidump data")
        return self.data[offset:offset + size]

    def unpack(self, offset, fmt):
        return struct.unpack(fmt, self.slice(offset, struct.calcsize(fmt)))

    def module_at(self, address):
        return next((module for module in self.modules if module.base <= address < module.base + module.size), None)

    def function_entry(self, address):
        """A stable RUNTIME_FUNCTION pointer from a matching PE exception directory."""
        module = self.module_at(address)
        if not module or not module.image_bytes:
            return None
        if module.base not in self.function_tables:
            data = module.image_bytes
            pe, = symbol_store.unpack(data, 60, "<I")
            rva, size = symbol_store.unpack(data, pe + 24 + 112 + 3 * 8, "<II")
            if size % 12 or size > len(data):
                raise ValueError("invalid PE exception directory")
            table = self.read(module.base + rva, size) if size else b""
            if len(table) != size:
                raise ValueError("unavailable PE exception directory")
            rows = list(struct.iter_unpack("<III", table))
            starts = [row[0] for row in rows]
            if starts != sorted(set(starts)) or any(not (begin < end <= module.size) for begin, end, _ in rows):
                raise ValueError("invalid PE runtime function ranges")
            self.function_tables[module.base] = (rows, starts, c.create_string_buffer(table))
        rows, starts, buffer = self.function_tables[module.base]
        index = bisect_right(starts, address - module.base) - 1
        if index >= 0 and address - module.base < rows[index][1]:
            return c.addressof(buffer) + index * 12
        return None

    def read(self, address, size):
        for base, count, offset in self.ranges:
            if base <= address and address + size <= base + count:
                return self.slice(offset + address - base, size)
        module = self.module_at(address)
        if module and module.image_bytes:
            data = module.image_bytes
            pe, = symbol_store.unpack(data, 60, "<I")
            count, = symbol_store.unpack(data, pe + 6, "<H")
            optional_size, = symbol_store.unpack(data, pe + 20, "<H")
            rva = address - module.base
            headers, = symbol_store.unpack(data, pe + 24 + 60, "<I")
            if rva + size <= headers:
                return data[rva:rva + size]
            for index in range(count):
                virtual_size, section_rva, raw_size, raw_offset = symbol_store.unpack(
                    data, pe + 24 + optional_size + index * 40 + 8, "<IIII")
                if section_rva <= rva and rva + size <= section_rva + max(virtual_size, raw_size):
                    offset = rva - section_rva
                    raw = data[raw_offset + offset:raw_offset + min(offset + size, raw_size)] if offset < raw_size else b""
                    return raw + b"\0" * (size - len(raw))
        return b""


def image_stamp(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE image")
    pe, = symbol_store.unpack(data, 60, "<I")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    stamp, = symbol_store.unpack(data, pe + 8, "<I")
    size, = symbol_store.unpack(data, pe + 24 + 56, "<I")
    return stamp, size


def attach_images(dump, store):
    store = Path(store)
    for module in dump.modules:
        key = f"{module.timestamp:08X}{module.size:x}"
        image = store / module.name / key / module.name
        if image.is_file():
            data = image.read_bytes()
            name, identity, _ = symbol_store.pe_identity(data)
            if (image_stamp(data) != (module.timestamp, module.size)
                    or (name, identity) != (module.pdb_name, module.pdb_key)):
                raise ValueError(f"retained image differs from dump: {module.name}")
            pdb = store / name / identity / name
            if symbol_store.inspect(pdb, symbol_store.pdb_identity) != identity:
                raise ValueError(f"retained PDB differs from dump: {module.name}")
            module.image, module.pdb, module.image_bytes = image, pdb, data
            continue
        # A matching local system image supplies unwind metadata even without
        # an OS PDB. Never follow paths supplied by the player's dump.
        if os.name == "nt" and module.name.lower().endswith(".dll"):
            candidate = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32" / module.name
            if candidate.is_file():
                data = candidate.read_bytes()
                if image_stamp(data) == (module.timestamp, module.size):
                    module.image, module.image_bytes = candidate, data


class NativeSymbols:
    def __init__(self, search_path=""):
        if os.name != "nt" or c.sizeof(PTR) != 8:
            raise RuntimeError("native dump symbolization requires 64-bit Windows Python")
        self.api = c.WinDLL("dbghelp", use_last_error=True)
        kernel = c.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = PTR
        self.process = kernel.GetCurrentProcess()
        signatures = {
            "SymInitializeW": ([PTR, c.c_wchar_p, U32], U32),
            "SymCleanup": ([PTR], U32), "SymSetOptions": ([U32], U32),
            "SymLoadModuleExW": ([PTR, PTR, c.c_wchar_p, c.c_wchar_p, U64, U32, PTR, U32], U64),
            "SymFromAddrW": ([PTR, U64, c.POINTER(U64), c.POINTER(SymbolInfo)], U32),
            "SymFunctionTableAccess64": ([PTR, U64], PTR),
            "SymGetModuleBase64": ([PTR, U64], U64),
            "MiniDumpReadDumpStream": ([PTR, U32, c.POINTER(PTR), c.POINTER(PTR), c.POINTER(U32)], U32),
            "StackWalk64": ([U32, PTR, PTR, c.POINTER(StackFrame), PTR, PTR, PTR, PTR, PTR], U32),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.api, name); function.argtypes = arguments; function.restype = result
        # Do not set SYMOPT_EXACT_SYMBOLS: it disables export fallback for OS
        # images without PDBs. Private GUID/age matching is checked before load.
        self.api.SymSetOptions(0x2 | 0x4 | 0x10 | 0x200 | 0x80000)
        if not self.api.SymInitializeW(self.process, search_path, False):
            raise c.WinError(c.get_last_error())

    def close(self):
        self.api.SymCleanup(self.process)

    def load(self, module):
        return self.api.SymLoadModuleExW(self.process, None, str(module.image), module.name,
                                         module.base, module.size, None, 0)

    def name(self, address):
        buffer = c.create_string_buffer(c.sizeof(SymbolInfo) + 2048 * 2)
        info = c.cast(buffer, c.POINTER(SymbolInfo))
        info.contents.SizeOfStruct = c.sizeof(SymbolInfo); info.contents.MaxNameLen = 2048
        displacement = U64()
        if self.api.SymFromAddrW(self.process, address, c.byref(displacement), info):
            return c.wstring_at(c.addressof(buffer) + SymbolInfo.Name.offset, min(info.contents.NameLen, 2047)), displacement.value
        return "", 0

    def frames(self, dump):
        # Validate native stream access against the already bounds-checked data.
        mapped = c.create_string_buffer(dump.data, len(dump.data))
        for kind in (4, 6):
            directory, stream, size = PTR(), PTR(), U32()
            if not self.api.MiniDumpReadDumpStream(mapped, kind, c.byref(directory), c.byref(stream), c.byref(size)):
                raise ValueError("DbgHelp could not read the dump's required streams")
        context = c.create_string_buffer(dump.context, len(dump.context))
        rip, = struct.unpack_from("<Q", dump.context, 248)
        rsp, rbp = struct.unpack_from("<QQ", dump.context, 152)
        frame = StackFrame()
        for field, value in ((frame.AddrPC, rip), (frame.AddrStack, rsp), (frame.AddrFrame, rbp)):
            field.Offset = value; field.Mode = 3
        callback_type = c.WINFUNCTYPE(U32, PTR, U64, PTR, U32, c.POINTER(U32))

        @callback_type
        def read_memory(process, address, output, size, read):
            del process
            try:
                data = dump.read(address, size)
                read[0] = len(data)
                if data: c.memmove(output, data, len(data))
                return bool(data)
            except (ValueError, struct.error):
                read[0] = 0
                return False

        invalid_metadata = False

        @c.WINFUNCTYPE(PTR, PTR, U64)
        def function_table(process, address):
            del process
            nonlocal invalid_metadata
            try:
                return dump.function_entry(address)
            except (ValueError, struct.error):
                invalid_metadata = True
                return None

        @c.WINFUNCTYPE(U64, PTR, U64)
        def module_base(process, address):
            del process
            module = dump.module_at(address)
            return module.base if module else 0

        addresses, seen = [rip], set()
        for _ in range(64):
            module = dump.module_at(addresses[-1])
            if not module or not module.image_bytes:
                break
            if not self.api.StackWalk64(0x8664, self.process, PTR(dump.thread_id or 1), c.byref(frame), context,
                    read_memory, function_table, module_base, None) or invalid_metadata:
                break
            state = (frame.AddrPC.Offset, frame.AddrStack.Offset)
            if not state[0] or state in seen or not dump.module_at(state[0]): break
            seen.add(state)
            if state[0] != addresses[-1]: addresses.append(state[0])
        return addresses


def symbolize(dump_path, store, llvm=None):
    if not Path(store).is_dir():
        raise ValueError("private symbol store must exist")
    dump = Dump(Path(dump_path).read_bytes())
    attach_images(dump, store)
    paths = sorted({str(module.pdb.parent) for module in dump.modules if module.pdb})
    if not paths:
        raise ValueError("no retained PDB matches the dump's module identities")
    if any(";" in path for path in paths):
        raise ValueError("private symbol directory contains a search-path separator")
    symbols = NativeSymbols(";".join(paths))
    try:
        for module in dump.modules:
            if module.image: symbols.load(module)
        output = [f"Exception {dump.code:#x} at {dump.address:#x} (thread {dump.thread_id})", "Exception-context stack:"]
        for index, address in enumerate(symbols.frames(dump)):
            module = dump.module_at(address)
            name, displacement = symbols.name(address)
            if llvm and module and module.pdb:
                name, displacement = llvm(module, address), 0
            label = module.name if module else "<unknown module>"
            detail = f"!{name}+{displacement:#x}" if name else f"+{address - module.base:#x}" if module else ""
            output.append(f"#{index:02d} {address:#018x} {label}{detail}")
        output.append("Only matching retained images/PDBs and matching local system images are used; unavailable unwind metadata can truncate the stack.")
        return "\n".join(output) + "\n"
    finally:
        symbols.close()
