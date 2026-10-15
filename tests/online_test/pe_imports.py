"""Read PE import tables without relying on the build's runtime-DLL generator expression."""
import pathlib
import struct

# Windows OS components, not redistributable compiler runtimes. In particular,
# MSVCP140/VCRUNTIME140 are deliberately absent and must be app-local.
SYSTEM_DLLS = set("""advapi32.dll bcrypt.dll bcryptprimitives.dll cabinet.dll cfgmgr32.dll
combase.dll comdlg32.dll crypt32.dll cryptbase.dll cryptsp.dll dbghelp.dll dnsapi.dll
fwpuclnt.dll gdi32.dll imm32.dll iphlpapi.dll kernel32.dll kernelbase.dll msimg32.dll
mswsock.dll ncrypt.dll netapi32.dll normaliz.dll nsi.dll ntdll.dll ole32.dll oleaut32.dll
powrprof.dll propsys.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll shlwapi.dll
sspicli.dll ucrtbase.dll user32.dll userenv.dll usp10.dll version.dll winhttp.dll wininet.dll
winmm.dll wintrust.dll ws2_32.dll wtsapi32.dll""".split())


def pe_imports(data):
    """Normal and delay imports, supporting both PE32 and PE32+. Fail closed on bad PE."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("bad PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:
        dirs, number = opt + 112, opt + 108
        image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    elif magic == 0x10B:
        dirs, number = opt + 96, opt + 92
        image_base = struct.unpack_from("<I", data, opt + 28)[0]
    else:
        raise ValueError("unsupported PE optional header")
    directory_count = struct.unpack_from("<I", data, number)[0]
    header_size = struct.unpack_from("<I", data, opt + 60)[0]
    sections = []
    for i in range(count):
        row = opt + opt_size + i * 40
        size, va, raw_size, raw = struct.unpack_from("<IIII", data, row + 8)
        sections.append((va, max(size, raw_size), raw_size, raw))

    def offset(rva, size=1):
        if 0 <= rva < header_size and rva + size <= min(header_size, len(data)):
            return rva
        for va, span, raw_size, raw in sections:
            if va <= rva < va + span:
                delta = rva - va
                if delta + size <= raw_size and raw + delta + size <= len(data):
                    return raw + delta
        raise ValueError("PE RVA is outside file-backed sections")

    imports = set()
    for index, descriptor_size in ((1, 20), (13, 32)):
        if directory_count <= index:
            continue
        rva, size = struct.unpack_from("<II", data, dirs + index * 8)
        if not rva:
            continue
        terminated = False
        for delta in range(0, size, descriptor_size):
            row = offset(rva + delta, descriptor_size)
            descriptor = struct.unpack_from("<" + "I" * (descriptor_size // 4), data, row)
            if not any(descriptor):
                terminated = True
                break
            name_rva = descriptor[3] if index == 1 else descriptor[1]
            if index == 13 and not (descriptor[0] & 1):
                name_rva -= image_base
            offset(name_rva)
            chars = bytearray()
            for n in range(260):
                char = data[offset(name_rva + n)]
                if not char:
                    break
                chars.append(char)
            else:
                raise ValueError("unterminated PE import name")
            name = chars.decode("ascii").lower()
            if not name or pathlib.PureWindowsPath(name).name != name:
                raise ValueError("invalid PE import name")
            imports.add(name)
        if not terminated:
            raise ValueError("unterminated PE import table")
    return imports


def verify_pe_imports(files):
    """files maps kit basenames to bytes, independent of CMake's expected DLL list."""
    names = {name.lower() for name in files}
    dependents = {}
    for name, data in files.items():
        if not name.lower().endswith((".exe", ".dll")):
            continue
        imports = pe_imports(data)
        missing = {d for d in imports if d not in names and d not in SYSTEM_DLLS
                   and not d.startswith(("api-ms-win-", "ext-ms-win-"))}
        if missing:
            raise AssertionError(f"{name}: imports not in the zip and not in the system set: {', '.join(sorted(missing))}; add to SYSTEM_DLLS if an OS component")
        dependents[name] = sorted(imports)
    return dependents
