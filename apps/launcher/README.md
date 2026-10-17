# EAWR.exe: the first-run launcher (Windows)

`EAWR.exe` (target `eawr_launcher`) is the file a player double-clicks. It is a small native GUI program:
no console window, no Python, the C++ runtime linked statically and only system DLLs imported, so a clean PC
needs no redistributable. It never elevates (`asInvoker`), never changes firewall rules and never writes into
the game folder.

## Package layout it expects

```text
EAWR.exe
godot\Godot_v4.7.2-stable_win64.exe     the Godot 4.7.2 GUI runtime (never the console companion)
project\project.godot ...               the viewer project with its extension
```

`--godot <exe>` and `--project <folder>` override the two relative paths (`EAWR_GODOT_EXECUTABLE` too). Without
`godot\...exe` it also looks beside itself and one folder up, and takes any `Godot_v4.7.2-stable*.exe` whose
name does not contain `console`. The packaging side (`tools/release/package_release.py`) puts the files there.

## What it does

1. **Finds Forces of Corruption**, in this order: `--game-root` / `EAWR_EAW_GAME_ROOT` (an explicit folder is
   never replaced by a search and never overwrites the saved choice), the choice saved by an earlier run (revalidated every run), then Steam (HKCU
   `SteamPath` and HKLM `InstallPath`, `libraryfolders.vdf` in the modern and the pre-2021 format,
   `appmanifest_32470.acf` (Gold Pack), any folder under `steamapps\common`), then GOG
   (`HKLM\SOFTWARE\GOG.com\Games` and the Galaxy / installer uninstall records, 32- and 64-bit views). A folder
   counts when `GameData\Data`, `corruption\Data` and `corruption\StarWarsG.exe` exist (names matched ignoring
   case, as `tools/release/demo.py` does); picking `corruption` or `GameData` selects their parent, and GOG-style
   wrapper folders are looked into one level down. One install is used silently. Several, or none, open a picker
   (a task dialog with one command link per install and a Browse button, then the system folder picker).
   `--choose-game`, or Shift held at startup, asks even when a choice is saved.
2. **Starts Godot hidden**: `Godot_v4.7.2-stable_win64.exe --path project -- --eawr-game-root <root>
   --eawr-front-end [--eawr-allow-unknown-build] --eawr-report <report>` from the launcher's folder with
   `CREATE_NO_WINDOW`. Fonts are extracted by the viewer itself, so no `--eawr-font-cache` is passed;
   `--allow-unknown-build` is forwarded as `--eawr-allow-unknown-build`. Anything after `--` (and unknown
   options) goes to the viewer, `--godot-quit-after <n>` becomes Godot's `--quit-after`.
3. **Explains failures**: a missing game, a bad folder, a missing runtime or project, a runtime that will not
   start, or Godot exiting with an error within a minute of starting opens a message box with the reason (the
   viewer's own text from its `--eawr-report` file when it wrote one) and the log paths. `--no-dialogs` sends the
   same text to stderr and never opens a window.

Exit codes: 0 played, 2 error, 3 cancelled in the picker, anything else is Godot's own code.

## Files

Godot's user-data folder for the viewer, `%APPDATA%\Godot\app_userdata\EAWR Viewer` (the folder the viewer's
`user://` uses; its name is the project's `config/name`, checked by `launcher_smoke`). `--user-data <folder>`
overrides it for tests. The launcher keeps its own files in the `launcher\` subfolder:

| File | Content |
|---|---|
| `launcher\launcher.json` | the saved game folder |
| `launcher\launcher.log` | what the launcher found and did (trimmed past 1 MiB) |
| `launcher\front-end-report.json` | the viewer's report of the last start (deleted before each start) |

The viewer's own `logs\godot.log` is named in the error dialogs.

## Layout of the code

`launcher_flow` holds the steps behind `Ui`, `RegistryReader` and `ProcessRunner` interfaces, so the tests run
them with fakes; `install_discovery` (VDF/registry search and validation), `launch_plan` (command line and
quoting), `launcher_state` (saved choice), `text_formats` (JSON and KeyValues parsers) and `paths` (UTF-8 and
case-insensitive paths) are portable standard C++; `win_*.cpp` are the registry, process, dialog and entry-point
layers.

## Tests

- `launcher_core_contracts` (`launcher_tests`): parsers, discovery against VDF and registry fixtures, Unicode
  and space paths, validation, the saved choice, the Godot command line round-trip and the whole flow with a
  fake UI and process runner.
- `launcher_smoke`: the built exe against `launcher_fake_godot` (records its argv and whether it has a console
  window): front-end arguments, hidden start, saved choice, readable errors; and checks on the exe itself (GUI
  subsystem, imports system DLLs only, `asInvoker` manifest).
- `launcher_ui_smoke`: the real dialogs, clicked on a private desktop (`tools/rig/private_desktop.py`).

Fixtures are empty folders and made-up bytes; no retail data.
