# Build and test

The root C++20 build remains engine-independent. It does not download or link Godot,
a renderer, graphics/audio libraries, game files, shaders, or anything under
`reference/`. All configure output goes under the ignored `out/` directory.

## Prerequisites

- CMake 3.25 or newer.
- Python 3.8 or newer for test drivers.
- Clang with `clang++` on `PATH` for the compiler-backed simulation boundary test,
  even when the project itself is compiled with MSVC or GCC.
- One build toolchain matching a preset:
  - `windows-msvc`: Visual Studio 2022 x64 MSVC.
  - `windows-clang`: Visual Studio 2022 x64 with the `ClangCL` toolset. This uses
    the MSVC-compatible `clang-cl` driver, not MinGW Clang.
  - `linux-x64-gcc`: GCC 14 (`g++-14`) and Ninja.
  - `linux-x64-clang`: Clang 18 (`clang++-18`) and Ninja.
  - `linux-arm64-gcc`: GCC 14 (`g++-14`) and Ninja on a native ARM64 host.

## Configure, build, and test

Run a complete configure/build/CTest workflow from the repository root:

```text
cmake --workflow --preset windows-msvc
cmake --workflow --preset windows-clang
cmake --workflow --preset linux-x64-gcc
cmake --workflow --preset linux-x64-clang
cmake --workflow --preset linux-arm64-gcc
```

Only run a preset on its named OS and architecture. The CI workflow runs all five.
The equivalent individual commands are:

```text
cmake --preset <preset>
cmake --build --preset <preset>
ctest --preset <preset>
```

The simulation-boundary checks parse sources with Clang. Configure tries a standalone
LLVM under Program Files (Windows only), then `clang++`/`clang-cl` on PATH, and keeps the
first one that compiles against the machine's C++ standard library; the Microsoft STL
rejects Clang releases older than the one it was built for. Set `EAWR_CLANGXX` to choose
one explicitly.

## Game fonts

The UI draws with the four EmpireAtWar faces that FoC embeds in its executable
(`docs/ui/ui-layer.md`, font-provenance decision D1). They are the foundry's work, so the project
never commits or ships them. Each player extracts them once from their own install:

```powershell
python tools/fonts/extract_eaw_fonts.py --game-root "<Star Wars Empire at War install>"
```

The script reads `corruption/StarWarsG.exe` under the game root (`--exe` names the file;
without either option it uses `EAWR_EAW_GAME_ROOT`). It validates every TrueType table
directory it finds (table records, every table checksum, the whole-font checksum
adjustment, the required tables and the name table). It writes `EmpireAtWar-Bold.ttf`,
`-Light.ttf`, `-Medium.ttf` and `-Stencil.ttf` plus a `fonts.json` manifest (sizes and
SHA-256 digests) to the ignored `out/fonts/`, or to `--out <dir>`. It prints names, sizes and
digests only. Other rules:

- Inside the repository it writes only to paths git ignores, and `.gitignore` ignores font
  files everywhere.
- Only the pinned FoC builds listed in the script extract by default. Any other executable
  fails with exit code 3; `--allow-unknown-build` extracts from it anyway, provided all
  four faces are present and valid.
- `--dry-run` scans without writing.

Exit codes: 0 extracted, 2 usage or unreadable input, 3 unknown executable, 4 faces
missing, invalid or ambiguous, 5 output refused or not written.

The viewer reads the cache from `--eawr-font-cache <dir>`, else `EAWR_FONT_CACHE`, else the
checkout's `out/fonts/`. A packaged viewer has no checkout, so pass the directory the
script wrote with `--out`. Without the cache, text falls back by rule UI-F3 to the default
Unicode face and then to the engine's own font.

## Selected Godot prototype

The retained presentation prototype is standalone so the root headless matrix stays
engine-independent. Install the pinned Godot 4.7.2 and godot-cpp 10.0.0 inputs, then
configure and test its C++ contracts from the repository root:

```powershell
pwsh prototypes/godot/install_dependencies.ps1
cmake -S prototypes/godot -B out/godot/build/windows
cmake --build out/godot/build/windows --config Release
ctest --test-dir out/godot/build/windows -C Release --output-on-failure
python tests/presentation/godot/test_contract.py
```

Run the pinned console editor from `out/godot/bin/windows/` with `--path
prototypes/godot/project`; do not use an unrelated installed Godot.NET build. The
ARM64 package helper and exact export/run arguments are documented in
`docs/prototypes/godot.md`. Its default ARM GNU path reuses the ignored historical
cache below `out/prototypes/sdl_wgpu/deps/`; that path is cache compatibility, not a
tracked SDL source dependency. Avoid cold Godot/template rebuilds when the verified
cache is available.

## Simulation boundary

CTest runs `tools/check_sim_boundary.py` with Clang's parsed C++ AST. The checker
discovers and prints every C/C++ file below `include/eawr/sim/` and `src/sim/`; an
unrecognised file extension below those roots is an error rather than an exclusion.
It rejects floating types after desugaring, `auto` that deduces a floating type,
floating expressions/literals, render/engine/platform includes, OS I/O includes,
and direct C/POSIX/Win32 file calls. Include paths are normalized before matching,
including backslashes and `.`/`..` path segments. Text in comments is not treated
as code.

To run it directly:

```text
python tools/check_sim_boundary.py --root . --clang clang++
python tests/core/test_sim_boundary.py --root . --clang clang++
python tests/acceptance/p0_01/run_boundary_supplemental.py --root . --clang clang++
```

The core and independent supplemental fixture tests expect each negative file to be
rejected for its named reason and each positive file to pass, so intentional
failures do not make the CI job red. Both run through CTest in every CI matrix job.

CI writes each CTest JUnit report to an absolute path below `out/test-results/` and
uploads that exact file. A missing report is a job failure rather than a warning.

## Warnings-as-errors proof

Project targets link the `eawr_project_warnings` interface. Imported dependencies
must be represented by imported targets/SYSTEM include paths and must not link this
interface, keeping external warnings separate without weakening project diagnostics.
CI proves `/WX` or `-Werror` on every matrix entry by injecting one deprecated call:

```text
python tools/verify_warnings_as_errors.py --root . --preset <preset> --config Release
```

The command succeeds only when the compiler build fails specifically on
`EAWR deliberate project warning probe`.
