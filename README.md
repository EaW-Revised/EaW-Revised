# EaW Revised

[![CI](https://github.com/EaW-Revised/EaW-Revised/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/EaW-Revised/EaW-Revised/actions/workflows/ci.yml?query=branch%3Amain)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL-3.0-or-later-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=cplusplus)](docs/build.md)
[![Godot 4.7.2](https://img.shields.io/badge/Godot-4.7.2-478CBF.svg?logo=godotengine&logoColor=white)](apps/viewer/README.md)
[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20the%20project-FF5E5B.svg?logo=ko-fi&logoColor=white)](https://ko-fi.com/mister_fordo)

An open-source, standalone engine reimplementation that runs **Empire at War: Forces of
Corruption** (FoC) from your own game installation.

The goal is to keep the look, feel and depth of the original game while building a
stronger foundation for larger battles, modern hardware and the modding community:

- **The complete experience:** space battles, land battles and galactic campaigns.
- **Existing mod compatibility:** load the game's data and XML/Lua mods directly, with the
  formats and behaviour mod authors rely on.
- **Modern rendering** of the original environments, ships, units and effects.
- **Better performance:** a multithreaded, deterministic simulation for demanding battles
  and ambitious mods.
- **Cross-platform:** Windows x64, Linux x64 and Linux ARM64, with a deterministic
  simulation as the foundation for multiplayer.

The project is in early development and is **not yet a playable replacement** for the
original game.

> **Not affiliated.** EaW Revised is an independent, non-commercial fan project, not
> affiliated with or endorsed by the makers or publishers of the original game. This
> repository contains no game files: no models, textures, sounds, fonts, scripts or
> shader source from the original game. You need your own legitimately purchased copy of Empire at War with the
> Forces of Corruption expansion, and the project reads its data from that installation.

If you enjoy the project and want to support its development, you can do so on
[Ko-fi](https://ko-fi.com/mister_fordo). Thank you!

## Status

The first milestone, rendering the world (FoC space and land maps with props, units,
particles, lighting, sky and shadows in a Godot-based viewer), is done. Current work is
the first playable space skirmish. The project targets Forces of Corruption only for now.

What works today:

- Reading a FoC installation: MEG archives and loose files through a virtual file system,
  the XML object registry, maps, models, textures and animations.
- A Godot viewer that renders FoC space and land maps from map and XML data.
- A headless deterministic simulation that builds a skirmish start from game data and
  replays recorded commands with identical state hashes on every platform.
- A Lua 5.0.2 script host and a sandboxed, deterministic Lua VM for gameplay scripts.

Not there yet: complete combat gameplay, the full HUD, campaigns and multiplayer.
The [roadmap](plan/backlog.md) and [phase plans](plan/) say what comes next.

## Quick start

### 1. Build and test the core

Prerequisites: CMake 3.25+, Python 3.8+, and one toolchain: Visual Studio 2022 (MSVC or
clang-cl) on Windows, or GCC 14 / Clang 18 with Ninja on Linux. Clang must also be on
`PATH` for the simulation boundary check ([build](docs/build.md)).

```text
git clone https://github.com/EaW-Revised/EaW-Revised.git
cd EaW-Revised
python -m pip install -r requirements-dev.txt     # optional test dependencies
cmake --workflow --preset linux-x64-gcc           # or windows-msvc, windows-clang, linux-x64-clang
```

The core builds and tests without the game. Tests that need game data report
**skipped** until you point them at your installation:

```text
# the folder that contains GameData/ and corruption/
export EAWR_EAW_GAME_ROOT="/path/to/your/game"     # PowerShell: $env:EAWR_EAW_GAME_ROOT = "..."
ctest --preset linux-x64-gcc
```

The game's fonts are not distributed either; extract them once from your own install
with `python tools/fonts/extract_eaw_fonts.py --game-root "<install>"`
([game fonts](docs/build.md#game-fonts)).

### 2. Fetch Godot and godot-cpp

The viewer is a C++ GDExtension for the pinned Godot 4.7.2 and godot-cpp 10.0.0. This
script downloads both (SHA-checked) into the ignored `out/godot/` folder:

```text
pwsh prototypes/godot/install_dependencies.ps1
```

### 3. Build and run the viewer

```text
cmake -S apps/viewer -B out/build/viewer -DEAWR_GODOT_CPP_ROOT=out/godot/deps/godot-cpp
cmake --build out/build/viewer --config Release --target eawr_viewer
```

Then open a FoC map with the pinned Godot (Windows shown; on Linux use
`out/godot/bin/linux/Godot_v4.7.2-stable_linux.x86_64`):

```text
out/godot/bin/windows/Godot_v4.7.2-stable_win64_console.exe --path apps/viewer/project -- --eawr-game-root "<install>" --eawr-map data/art/maps/_mp_land_naboo.ted --eawr-populate
```

The [viewer guide](apps/viewer/README.md) lists every mode (maps, units, effects, the live
skirmish session) and option.

## How it works

**Deterministic lockstep simulation.** Authoritative state uses checked 64-bit Q24 fixed
point with defined rounding and no floating point ([fixed point](docs/fixed-point.md)). A
[boundary check](docs/build.md#simulation-boundary) rejects floating-point types and engine
or OS includes in the simulation. Headless replay hashes are the cross-platform
determinism oracle ([replay format](docs/replay-format.md)).

**Multithreaded, identical for any worker count.** Per-entity work runs in named phases
over fixed partitions on a worker pool: workers read immutable copies and write disjoint
staging slots, and one ordered serial commit changes state. 1, 2, 4, 8 or all hardware
threads give the same hashes ([simulation](docs/simulation.md)).

**Deterministic Lua.** The authoritative [Lua sandbox](docs/lua-sandbox.md) fixes
iteration order, script time and random streams, so scripts can take part in lockstep.

**Clean-room reimplementation.** Original behaviour is established by observing the
running game and by studying the game's own debug build and published reference
material. Findings become plain-language [behaviour notes](docs/behaviour/README.md) with
rule IDs and test cases, and code is written from those notes
([clean-room rule](docs/clean-room.md)).

**Data-driven mod compatibility.** All I/O goes through a case-insensitive
[virtual file system](docs/vfs.md) that layers mods over the expansion over the base game.
Original XML tag names and Lua API names keep their meaning; unsupported features are
reported rather than guessed ([XML model](docs/xml-model.md)).

**Presentation apart from simulation.** Immutable snapshots are the only path from the
simulation to the renderer; Godot types never enter authoritative state
([architecture decisions](docs/architecture-decisions.md)).

## Repository layout

```text
apps/          Executables: viewer (Godot GDExtension), sim_headless, benchmarks, scanners
include/eawr/  Public headers of the engine-free core
src/           Core libraries: vfs, data, assets, scene, units, sim, skirmish, script,
               presentation, platform
tests/         CTest suites, fidelity scenarios, fixtures (all synthetic or our own)
tools/         Python tooling: comparers, inventories, shader translation, fonts
docs/          Contracts, decisions and behaviour notes (index: docs/README.md)
plan/          Roadmap, phase plans and data inventories
prototypes/    The retained Godot rendering prototype and the Godot fetch script
third_party/   Vendored EnTT, Lua 5.0.2 and pugixml (THIRD_PARTY_NOTICES.md)
cmake/         Shared CMake modules
```

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first: it
covers the clean-room rules (never paste decompiler output, binary addresses, original
shader source or game files), the DCO sign-off, and running the tests without game data.
Security issues go through [SECURITY.md](SECURITY.md). Everyone taking part follows the
[code of conduct](CODE_OF_CONDUCT.md).

## Licence

The project's own code is released under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)).
Third-party code keeps its own licence; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
