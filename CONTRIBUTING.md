# Contributing to EaW Revised

Thank you for helping. This page covers the rules that keep the project legally clean,
how to sign off your work, and how to build and test without the game.

## The clean-room rules

EaW Revised is a reimplementation, not a copy. Every contribution must follow these rules;
a pull request that breaks them is closed, and its content removed from history if needed.

**Never commit or paste:**

- game files or anything extracted from them: models, textures, sounds, music, fonts,
  videos, maps, XML or Lua files from the game, or screenshots and recordings of the
  original game;
- decompiler or disassembler output, binary addresses, or the original engine's class,
  method or field names (from a debugger, symbol file or decompiler);
- source code from leaked or "confidential" releases of the original engine, or the
  published original shader source (`.fx` / `.fxh` files);
- anything you cannot license under this project's licence.

**How original behaviour gets in:**

1. Someone observes the original game (playing it, recording it, reading its data files,
   or using its own debug tools) and writes a **behaviour note** under `docs/behaviour/`:
   the rule in plain language, in your own words and structure, with a rule ID
   (for example `BP-02`) and test cases. Say what the evidence is ("observed in the
   game", "debug build", "game data").
2. Code is written from the note in the project's own structure and naming, and cites
   the rule ID in comments.
3. If you are not sure about a behaviour, mark it "unverified" in the note and choose the
   least visible option; do not implement a guess as fact.

Names that mods use are the modding interface and may appear: XML tag names, Lua API
function names, shader parameter names that the game's data binds to, and data file names.

CI runs `python tools/cleanroom_check.py` over the whole tree; it flags engine-style names
(class names ending in "Class" or "Wrapper", "al"-prefixed engine types, "m_" member
fields) and binary addresses.

## Sign your commits (DCO)

Every commit must carry a `Signed-off-by` line, certifying the
[Developer Certificate of Origin](https://developercertificate.org/): you wrote the
change, or have the right to submit it under the project's licence. Use `git commit -s`,
which adds a line with your configured name and email:

```text
Signed-off-by: Your Name <you@example.com>
```

The DCO check in CI fails a pull request with an unsigned commit; `git rebase --signoff
main` adds the line to existing commits.

## Build and test

See the [README quick start](README.md#quick-start) and [docs/build.md](docs/build.md).
In short:

```text
python -m pip install -r requirements-dev.txt
cmake --workflow --preset linux-x64-gcc      # or windows-msvc, windows-clang, linux-x64-clang
```

- **Without the game** the whole suite runs; tests that need FoC data report **skipped**,
  not passed. Set `EAWR_EAW_GAME_ROOT` to your installation (the folder with `GameData/`
  and `corruption/`) to run them too. Nothing is ever written into the installation.
- **Graphical viewer tests** are opt-in: they need a GPU, the pinned Godot
  (`EAWR_GODOT_EXECUTABLE`) and `EAWR_GODOT_VIEWER_RUNTIME_TEST=1`. CI does not run them.
- Warnings are errors in project code, on every compiler.

## What a good pull request looks like

- **Small and focused**, with a description of what changed and why, and which behaviour
  note rule IDs it implements.
- **Tests** for new behaviour. Tests use synthetic fixtures or our own data; a test that
  needs the game reads it from `EAWR_EAW_GAME_ROOT` and skips without it.
- **Determinism kept.** Simulation changes must give identical results for 1, 2, 4 and 8
  worker threads and on every platform. If a change intentionally moves the replay hashes,
  regenerate the pinned hashes with the project's pin tools and say which pins moved and why.
- **Data first.** Game behaviour is driven by the XML data: read values through the
  validated loaders instead of hard-coding them, and update the tag registry
  (`docs/tag-coverage.md`) when your change starts applying a tag.
- **Presentation-only changes** (viewer, rendering, UI, audio) leave the headless replay
  hashes byte-identical.

## Questions and ideas

Open an issue with the bug report or feature request template. For a larger change, open
an issue first so we can agree on the approach before you spend time on it.
