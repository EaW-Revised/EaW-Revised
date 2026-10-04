#!/usr/bin/env python3
"""Start the packaged skirmish setup (or, with --m2, the fixed M2 battle) using the player's own installation and fonts."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import extract_eaw_fonts as fonts

ROOT = Path(__file__).resolve().parent
GODOT_VERSION = "4.7.2.stable"


def steam_roots():
    """Discover Steam libraries without assuming a drive or account name."""
    candidates = [Path.home() / ".steam/steam", Path.home() / ".local/share/Steam"]
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
                candidates.append(Path(winreg.QueryValueEx(key, "SteamPath")[0]))
        except OSError:
            pass
    for steam in candidates:
        yield steam
        libraries = steam / "steamapps/libraryfolders.vdf"
        if libraries.is_file():
            for value in re.findall(r'"path"\s+"([^"]+)"', libraries.read_text(encoding="utf-8", errors="replace")):
                yield Path(value.replace("\\\\", "\\"))


def game_candidates():
    for steam in steam_roots():
        common = steam / "steamapps/common"
        if common.is_dir():
            yield from common.iterdir()


def game_root(path):
    path = Path(path).expanduser().resolve()
    fonts.locate_executable(path)
    # The VFS expects the parent containing both the base and expansion folders.
    root = path.parent if path.name.casefold() == "corruption" else path
    for directory in ("GameData/Data", "corruption/Data"):
        if not (root / directory).is_dir():
            raise ValueError("FoC installation is missing " + str(root / directory))
    return root


def godot_candidates():
    for folder in (ROOT, ROOT / "godot", ROOT.parent):
        # The Windows console binary keeps startup errors visible in the launcher.
        yield from sorted(folder.glob("Godot_v4.7.2-stable*"),
                          key=lambda path: ("console" not in path.stem.casefold(), str(path)))
    for name in ("godot", "godot4", "Godot_v4.7.2-stable_win64_console.exe"):
        found = shutil.which(name)
        if found:
            yield Path(found)


def godot_binary(path):
    executable = shutil.which(str(path)) or str(Path(path).expanduser().resolve())
    result = subprocess.run([executable, "--headless", "--version"], capture_output=True, text=True, timeout=20)
    if result.returncode or not result.stdout.strip().startswith(GODOT_VERSION + "."):
        raise ValueError("Use the standard Godot 4.7.2 binary (not a different version).")
    return Path(executable)


def interactive_stdin():
    if not sys.stdin.isatty():
        return False
    if os.name == "nt":
        # isatty() also accepts character devices such as NUL on Windows.
        import ctypes
        import msvcrt
        mode = ctypes.c_ulong()
        handle = ctypes.c_void_p(msvcrt.get_osfhandle(sys.stdin.fileno()))
        return bool(ctypes.windll.kernel32.GetConsoleMode(handle, ctypes.byref(mode)))
    return True


def choose(explicit, candidates, validate, prompt, option):
    if explicit:
        return validate(explicit.strip('"'))
    for candidate in candidates:
        try:
            return validate(candidate)
        except (OSError, ValueError, fonts.ExtractionError, subprocess.SubprocessError):
            continue
    if not interactive_stdin():
        raise ValueError("No valid path found; supply " + option + " <path> (stdin is not an interactive console).")
    while True:
        answer = input(prompt + ": ").strip().strip('"')
        if not answer:
            raise ValueError("No path supplied; run the launcher again when ready.")
        try:
            return validate(answer)
        except (OSError, ValueError, fonts.ExtractionError, subprocess.SubprocessError) as error:
            print(error, file=sys.stderr)


def look_args(cache):
    """The owner's look shared by the setup screen and the fixed battle."""
    return ["--eawr-map-effects", "on", "--eawr-lighting", "sh", "--eawr-environment", "map",
            "--eawr-shadows", "on", "--eawr-live-ai", "on", "--eawr-hud", "tactical",
            "--eawr-audio", "on", "--eawr-font-cache", str(cache)]


def setup_args(game, cache):
    # The setup screen picks the map and builds its camera from it; no fixed map or config path.
    return ["--eawr-game-root", str(game), "--eawr-skirmish-setup"] + look_args(cache)


def battle_args(root, game, cache):
    return ["--eawr-game-root", str(game), "--eawr-map", "data/art/maps/_mp_space_coruscant.ted",
            "--eawr-populate", "--eawr-camera-interactive", "--eawr-live-session", "m2"] + look_args(cache) + [
            "--eawr-map-camera-config", str(root / "project/config/coruscant-live-session-camera.xml")]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", help="install folder containing GameData and corruption")
    parser.add_argument("--godot", help="path to the standard Godot 4.7.2 executable")
    parser.add_argument("--allow-unknown-build", action="store_true", help="validate fonts from an unpinned FoC executable")
    parser.add_argument("--godot-quit-after", type=int, help="quit after N frames (smoke checks)")
    parser.add_argument("--m2", action="store_true",
                        help="skip the skirmish setup screen and start the fixed M2 Coruscant battle")
    args, extra = parser.parse_known_args(argv)
    if extra[:1] == ["--"]:
        extra = extra[1:]
    try:
        state_file = ROOT / "out/demo.json"
        try:
            state = json.loads(state_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        if not isinstance(state, dict):
            state = {}
        game = choose(args.game_root or os.environ.get("EAWR_EAW_GAME_ROOT"),
                      [state.get("game_root", "")] + list(game_candidates()), game_root,
                      "Folder containing your FoC installation (GameData and corruption)", "--game-root")
        godot = choose(args.godot or os.environ.get("EAWR_GODOT_EXECUTABLE"),
                       [state.get("godot", "")] + list(godot_candidates()), godot_binary,
                       "Path to the downloaded Godot 4.7.2 executable", "--godot")
        cache = ROOT / "out/fonts"
        # Revalidate against this installation, including checksums; never reuse foreign fonts.
        fonts.extract(fonts.locate_executable(game), cache, args.allow_unknown_build, False)
        state_file.write_text(json.dumps({"game_root": str(game), "godot": str(godot)}, indent=2) + "\n", encoding="utf-8")
        engine_args = [str(godot), "--path", str(ROOT / "project")]
        if args.godot_quit_after is not None:
            engine_args += ["--quit-after", str(args.godot_quit_after)]
        if args.m2:
            print("Starting the space battle. Close the window to quit.", flush=True)
            viewer_args = battle_args(ROOT, game, cache)
        else:
            print("Starting the skirmish setup. Close the window to quit.", flush=True)
            viewer_args = setup_args(game, cache)
        return subprocess.call(engine_args + ["--"] + viewer_args + extra, cwd=ROOT)
    except (OSError, ValueError, EOFError, fonts.ExtractionError, subprocess.SubprocessError) as error:
        print("play-demo: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
