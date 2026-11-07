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
`godot\...exe` it also looks beside the launcher (the parent folder and other `Godot_v4.7.2-stable*.exe` names are
not searched). The packaging side (`tools/release/package_release.py`) puts the files there.

### Engine check (we only run our patched Godot)

Before every start the launcher hashes the engine it is about to run (SHA-256, 1 MiB chunks, once per launch) and
compares it with `godot_sha256` in `runtime.json` next to `EAWR.exe`. A `--godot` / `EAWR_GODOT_EXECUTABLE`
override is checked the same way. Without an override, the first candidate (`godot\` then the launcher folder)
whose hash matches wins; if none matches, the first existing candidate still starts, flagged.

| Engine | `launcher.log` | Viewer command line |
|---|---|---|
| hash equals `godot_sha256` | nothing | no flag |
| hash differs | `EAWR-ENGINE-UNPATCHED: <exe> has SHA-256 <actual>, runtime.json records <expected>; starting anyway, flagged` | `--eawr-engine-unverified` |
| `runtime.json` or its hash missing/unreadable/malformed | same line with `(none: ...)` as the expected hash | `--eawr-engine-unverified` |
| engine file unreadable | same line with `(unreadable)` as the actual hash | `--eawr-engine-unverified` |

The check never stops play. The viewer reads `--eawr-engine-unverified`: it shows a red UNPATCHED ENGINE warning, and
test/gate runs exit with code 7.

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
   `--allow-unknown-build` is forwarded as `--eawr-allow-unknown-build`. The project's default is the Separate render thread;
   `--safe-render-thread` adds Godot's `--render-thread safe` right after `--path` (the opt-out; the player zip's `EAWR (Safe render thread).cmd` uses it). Anything after `--` (and unknown
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
quoting) and `launcher_state` (saved choice) are portable standard C++; `win_*.cpp` are the registry, process,
dialog and entry-point layers. The JSON and KeyValues parsers, the UTF-8 and case-insensitive path helpers and the
`libraryfolders.vdf` reader live in `src/mods/steam` (shared with the mod core); the launcher compiles those
sources itself with its static C runtime instead of linking `eawr_mods`, and `src/text_formats.hpp` and
`src/paths.hpp` keep the launcher's names for them.

## Tests

- `launcher_core_contracts` (`launcher_tests`): discovery against VDF and registry fixtures, Unicode
  and space paths, validation, the saved choice, the Godot command line round-trip and the whole flow with a
  fake UI and process runner. The parser cases run in `mods_contracts` (`tests/mods`).
- `launcher_smoke`: the built exe against `launcher_fake_godot` (records its argv and whether it has a console
  window): front-end arguments, hidden start, saved choice, readable errors; and checks on the exe itself (GUI
  subsystem, imports system DLLs only, `asInvoker` manifest).
- `launcher_ui_smoke`: the real dialogs, clicked on a private desktop (`tools/rig/private_desktop.py`).

Fixtures are empty folders and made-up bytes; no retail data.

The Performance **Render thread (restart)** setting persists in Godot's `user://eawr-settings.cfg` and applies at the next launcher start. For the bundled project this is `%APPDATA%\Godot\app_userdata\EAWR Viewer\eawr-settings.cfg` on Windows, or `$XDG_DATA_HOME/godot/app_userdata/EAWR Viewer/eawr-settings.cfg` on Linux (defaulting to `~/.local/share` for `$XDG_DATA_HOME`). The launcher reads that same file even when `--user-data` puts its own state and logs elsewhere. `--render-thread safe|separate` (or `--safe-render-thread`) overrides the saved choice. With no saved choice or flag, the project default applies. An explicit `--eawr-settings-file` forwarded to the viewer selects the same settings file for startup. Relative overrides resolve from the launcher folder, matching Godot's working directory; an empty final override restores the default user:// file. Direct engine launches use their explicit engine flag or project default; they do not read this startup preference themselves. Shipped Safe start files and the owner comparison kit deliberately select a mode explicitly.
For manager-selected profiles, render-thread precedence is explicit CLI mode > profile mode > saved Performance setting > project default. Both explicit Safe and explicit Separate override a profile. A plain EAWR start remains CLI > saved Performance setting > project default.

With a bundled manager, `EAWR.exe --mods` starts EAWR Mods and passes
`--eawr-launch-loop <user-data>/launcher/next-action.json`. A successful child exit
may request `{"action":"launch-profile","profile":"<id>"}` or
`{"action":"open-manager"}`. The launcher consumes the request before starting
the next process and clears stale requests before every start. Missing or invalid
requests end the launcher normally; nonzero child exits retain their exit code.
If an old request cannot be deleted, startup stops with a readable error, including
on a plain EAWR start, so a stale request can never trigger a later launch.
More than 16 immediate restart requests stop with an error; a game session lasting
at least 60 seconds resets that guard.

Profile launches use `mods/profiles/<id>.json` under the launcher user-data folder,
pass `--eawr-mod-profile` to the game and put the profile's `--render-thread safe|separate`
before Godot's `--` separator. An explicit `--safe-render-thread` overrides the
profile setting. The launcher remembers `last_profile` in
`launcher/launcher.json`; a plain EAWR start keeps its existing front-end behaviour.
The engine remains responsible for loading and validating the full profile.

`--user-data` is not yet supported together with `--mods`: the manager currently
starts the profile service with its default user-data folder. Use the default
folder for manager launches until the manager forwards this option to the service.

Manager-equipped kits include `EAWR Mods.exe`, a native GUI program with a static C runtime.
Double-click it to start `EAWR.exe --mods` from the stub's own folder without a console.
Run `"EAWR Mods.exe" --create-shortcut` to create an **EAWR Mods** desktop shortcut pointing
at the stub. It checks for an incomplete kit and asks before replacing an existing shortcut,
with **No** selected by default. The start executable keeps working when the kit moves;
recreate the desktop shortcut after moving the kit. No script host is required.

### Stay-open launch contract

EAWR passes `--eawr-launch-loop-stay-open <user-data>/launcher/game-running.json`
to the manager alongside `--eawr-launch-loop`. A manager that supports this
contract atomically writes `{"action":"launch-profile","profile":"<id>","stay_open":true}`
to the request path and stays open. It should disable further launches while the
request is pending or a game is running. EAWR consumes the request, starts the
game, and atomically publishes `{"profile":"<id>","pid":<game process id>,"process_start_time":"<creation time>"}`
at the supplied state path. The manager can use the profile ID to block edits
to the active profile and the PID to recognize a stale file after a crash. The
file is removed when the game exits; if EAWR is forcibly terminated, the file
can remain. At startup EAWR removes a state whose process is known to have exited.
The optional `process_start_time` is the Windows process creation time as a decimal
string of 100-nanosecond FILETIME ticks; when available, EAWR compares it to
the live process to detect PID reuse. Older files without it remain valid, and
unreadable, malformed, or indeterminate states are left in place. Before making a request, the manager removes any old `launch-result.json` in the
same directory, blocks edits to that profile and disables further launches. It
keeps that pending lock while waiting for either the running-state file or a
new result, including the interval between request consumption and publication.
When the game exits or a request fails, EAWR atomically writes
`launch-result.json` as `{"profile":"<id>","status":"finished"|"failed","exit_code":0}`;
the exit code is the game exit code when available and zero for a failed start
or invalid request. A malformed profile ID is reported as an empty string.
The result completes the pending request even if the game exits too quickly for
the manager to observe the running state. Other applications
can still edit the profile while the game starts, so the manager must lock edits
before submitting the request and keep them locked until completion.
Requests without `stay_open: true` retain the exit-then-launch behavior for
older managers. The manager must write requests atomically, and should not
submit another request until the current game has exited.

## Several game instances (contract v2) (legacy EAWR-5619)

`EAWR.exe --instance <n|auto> [--profile <id>]` runs one more game next to the first. The single-slot files above
are the contract for **instance 1**, which is unchanged: no `--instance`, or `--instance 1`, behaves exactly as
before (same folders, same files, no extra game flags). Instances 2 to 8 (`kMaxInstanceId`; the manager's own cap
is lower, owner decision: 4) get a folder of their own and a redirected `APPDATA`.

```text
<user-data>\instances\<n>\
  launcher\   next-action.json, game-running.json, launch-result.json, front-end-report.json, launcher.log
  appdata\    Godot\app_userdata\EAWR Viewer\   the game's user:// (APPDATA of the game process)
  .claim       {"pid":<EAWR.exe>,"process_start_time":"<FILETIME ticks>"}  who owns the slot
```

- `--instance auto` takes the lowest free id from 2 (gaps are reused). The claim is created with an exclusive
  publish (a complete file appears or nothing), so two EAWR.exe started at the same moment get different ids.
  A claim is stale when its process is gone, the PID now belongs to a process with another creation time, or the
  file is empty or corrupt; the next start of that id reuses the slot and logs "reused a stale slot claim" once.
  A live owner refuses a second start of the same id with a readable message. The claim is removed when EAWR.exe
  exits (a killed EAWR.exe leaves it; it is stale by then).
- Only the game child gets `APPDATA=<instances\<n>\appdata>`; the rest of its environment is the inherited one
  unchanged, and the manager (`--mods`) never gets it. EAWR.exe itself keeps reading the real folder.
- Request, state, report and result files are only ever read, written or removed in the folder of the caller's own
  instance. A second EAWR.exe (or a second launcher window with `--mods --instance <n>`) no longer clears the first
  one's `next-action.json` or `front-end-report.json`.
- On the first start of an id, `eawr-settings.cfg` and `eawr-profile.cfg` are copied from the first instance's
  `user://` folder (nothing else): `video.windowed` is forced to 1, and the lobby name becomes `"<name> #n"`
  (`player.network_name`, else `player.name`, else `Player`; cut so the whole name stays within 32 characters).
  Files that already exist are never overwritten; a missing primary file is replaced by a minimal one.
- Shared with the first instance, by flags: saves (`--eawr-save-dir <primary user://>\saves`), the font cache
  (`--eawr-font-cache <primary user://>\fonts`) and the mod profile (`--eawr-mod-profile`, full path under the
  first instance's `mods\profiles`; `launcher.json` and the profiles stay in the first instance's folder).
  Per instance: logs, replays, LAN desync dumps, the engine's shader cache.
- The LAN game port of instance n is `47624 + 2*(n-1)` (47624, 47626, ...); EAWR.exe passes it as
  `--eawr-lan-game-port <port>` and records it as `host_port` in `game-running.json`. 47625 stays LAN discovery.
  Joiners on one PC type `127.0.0.1:<port>`. (The viewer accepts the flag in real play in a separate change.)
- `--profile <id>` selects `mods\profiles\<id>.json` before the first start (same checks as a manager request) and
  starts the game directly, with no manager; it cannot be combined with `--mods`. A missing or invalid profile
  stops with a readable error and starts nothing.
- An extra instance without `--mods` plays one game and exits with the game's exit code. While it runs it
  publishes `game-running.json` in its folder, `{"profile":"<id or empty>","pid":N,"process_start_time":"...",
  "instance":n,"host_port":P}`, and when the game ends it removes that file and writes `launch-result.json`
  (`{"profile":...,"status":"finished"|"failed","exit_code":N,"instance":n}`). The additive `instance` and
  `host_port` fields also appear in the files of a manager (`--mods --instance <n>`) that starts games.
  Instance 1 files keep their v1 shape.
- `--instance` outside 1 to 8 (0, -3, abc, 99) is a command-line error (exit 2).

Tests: `launcher_core_contracts` (id parsing, paths, ports, environment block, seeding, claims including stale and
PID reuse, concurrent claims, golden flag set, flow with a fake runner) and `launcher_smoke` (real EAWR.exe against
`launcher_fake_godot`: APPDATA and flags of instance 2, three concurrent `--instance auto`, a killed instance).
