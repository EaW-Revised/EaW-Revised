"""Compile tiny engine-free fixtures with a coherent compiler/STL pair.

Windows uses the VS 2022 x64 toolchain, as the project's build presets do.
Do not let a PATH clang++ independently discover a newer Visual Studio STL.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import subprocess


@dataclass
class Toolchain:
    compiler: str
    environment: dict[str, str]
    msvc: bool = False
    stl_version: str = "platform default"


def select_toolchain() -> Toolchain | None:
    environment = dict(os.environ)
    if os.name != "nt":
        compiler = environment.get("EAWR_CLANGXX") or shutil.which("clang++") or shutil.which("g++")
        return Toolchain(compiler, environment) if compiler else None

    environment = {key.upper(): value for key, value in environment.items()}
    vswhere = Path(environment.get("PROGRAMFILES(X86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError("Native fixture requires VS 2022 C++ x64 tools; vswhere.exe is missing")
    found = subprocess.run([str(vswhere), "-products", "*", "-version", "[17.0,18.0)",
                            "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True, timeout=30)
    if found.returncode or not found.stdout.strip():
        raise RuntimeError("Native fixture requires a complete VS 2022 C++ x64 toolchain: " + found.stderr)
    root = Path(found.stdout.strip())
    vcvars = root / "VC/Auxiliary/Build/vcvars64.bat"
    # A CTest can inherit a different developer shell. Reinitialize its compiler,
    # include and library settings in the child only, leaving the caller untouched.
    for key in list(environment):
        upper = key.upper()
        if upper.startswith(("VSCMD_", "VCTOOLS", "WINDOWSSDK", "UCRT")) or upper in {
            "VSINSTALLDIR", "VCINSTALLDIR", "DEVENVDIR", "INCLUDE", "LIB", "LIBPATH", "CL", "_CL_",
        }:
            del environment[key]
    # cmd.exe does not understand list2cmdline's backslash-escaped quotes.
    command = f'"{environment.get("COMSPEC", "cmd.exe")}" /d /s /c "call "{vcvars}" >NUL && set"'
    initialized = subprocess.run(command,
                                 env=environment, capture_output=True, text=True, timeout=30)
    if initialized.returncode:
        raise RuntimeError("Native fixture could not initialize VS 2022 x64 tools: " + initialized.stderr)
    # Environment names on Windows are case insensitive; normalize the captured
    # names to avoid duplicate PATH/Path entries when passing them back to Windows.
    environment = {key.upper(): value for line in initialized.stdout.splitlines()
                   if "=" in line for key, value in [line.split("=", 1)] if key}
    tools = Path(environment.get("VCTOOLSINSTALLDIR", ""))
    compiler = tools / "bin/Hostx64/x64/cl.exe"
    if not tools.is_absolute() or not tools.resolve().is_relative_to(root.resolve()) or not compiler.is_file():
        raise RuntimeError(f"Native fixture VS environment selected an invalid compiler: {compiler}")
    version = environment.get("VCTOOLSVERSION", tools.name)
    override = os.environ.get("EAWR_CLANGXX")
    if override:
        # Only a driver bundled with this instance can use the captured STL and
        # linker environment. GNU-style clang++ does its own VS discovery.
        candidate = Path(override if Path(override).is_absolute() else shutil.which(override) or override)
        allowed = [compiler, root / "VC/Tools/Llvm/x64/bin/clang-cl.exe"]
        if candidate.resolve() not in [path.resolve() for path in allowed] or not candidate.is_file():
            raise RuntimeError(f"Native fixture EAWR_CLANGXX={override} does not match VS 2022 STL {version}; "
                               "unset EAWR_CLANGXX or select this instance's x64 cl.exe/clang-cl.exe")
        compiler = candidate
    return Toolchain(str(compiler), environment, True, version)


def compile_fixture(toolchain: Toolchain, source: Path, executable: Path) -> None:
    if toolchain.msvc:
        arguments = ["/std:c++20", "/EHsc", str(source), f"/Fe:{executable}", f"/Fo:{source.with_suffix('.obj')}"]
    else:
        arguments = ["-std=c++20", str(source), "-o", str(executable)]
    built = subprocess.run([toolchain.compiler, *arguments], env=toolchain.environment,
                           cwd=source.parent, capture_output=True, text=True, timeout=60)
    if built.returncode:
        # cl's banner names its version; clang-cl needs --version separately.
        version = subprocess.run([toolchain.compiler, "--version" if "clang" in Path(toolchain.compiler).name
                                  or not toolchain.msvc else "/Bv"], env=toolchain.environment,
                                 cwd=source.parent, capture_output=True, text=True, timeout=30)
        raise RuntimeError(f"Native fixture compiler {toolchain.compiler}; STL {toolchain.stl_version}; "
                           "compiler/STL must be compatible:\n" + version.stdout + version.stderr +
                           built.stdout + built.stderr)
