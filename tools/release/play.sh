#!/bin/sh
# EaW Revised, Linux x64 player: starts the game with the bundled patched Godot engine.
#
#   ./play.sh [--game-root <folder holding GameData and corruption>] [--check] [-- <extra game arguments>]
#
# The game root is, in this order: --game-root, $EAWR_GAME_ROOT, $EAWR_EAW_GAME_ROOT, then the Steam libraries
# (~/.steam/steam and ~/.local/share/Steam, including the extra library folders Steam lists).
# --check only runs the checks (platform, glibc, engine hash, game folder) and exits without starting the game.
# No retail data is part of this folder; the game reads your own copy and never writes beside the player folder.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
engine="$here/godot/godot.linuxbsd.editor.x86_64"
project="$here/project"
runtime="$here/runtime.json"
need_glibc_major=2
need_glibc_minor=38

fail() { printf 'play.sh: %s\n' "$1" >&2; exit "${2:-2}"; }

game_root=${EAWR_GAME_ROOT:-${EAWR_EAW_GAME_ROOT:-}}
check_only=0
while [ $# -gt 0 ]; do
    case $1 in
        --game-root) [ $# -ge 2 ] || fail "--game-root needs a folder"; game_root=$2; shift 2 ;;
        --game-root=*) game_root=${1#--game-root=}; shift ;;
        --check) check_only=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done

# (a) platform and glibc
[ "$(uname -s)" = Linux ] || fail "this player is for Linux"
[ "$(uname -m)" = x86_64 ] || fail "this player is for 64-bit x86 Linux (x86_64); this machine reports $(uname -m)"
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | sed 's/^glibc //' || true)
if [ -n "$glibc" ]; then
    major=${glibc%%.*}; rest=${glibc#*.}; minor=${rest%%.*}
    if [ "$major" -lt "$need_glibc_major" ] 2>/dev/null || { [ "$major" -eq "$need_glibc_major" ] && [ "$minor" -lt "$need_glibc_minor" ]; } 2>/dev/null; then
        fail "this player needs glibc $need_glibc_major.$need_glibc_minor or newer (Ubuntu 24.04, Debian 13, Fedora 39 or newer); this system has glibc $glibc"
    fi
fi

# exec bit: some extract tools drop the mode
if [ ! -x "$engine" ]; then
    chmod +x "$engine" 2>/dev/null || fail "$engine is not executable and the mode cannot be set: run chmod +x on it"
fi
[ -d "$project" ] || fail "the project folder is missing next to play.sh: $project"

# (c) the engine must be the pinned patched build; a mismatch is passed on so the game labels the run unpatched
unverified=
if [ -f "$runtime" ] && command -v sha256sum >/dev/null 2>&1; then
    expected=$(sed -n 's/.*"godot_sha256": *"\([0-9a-f]\{64\}\)".*/\1/p' "$runtime" | head -n 1)
    found=$(sha256sum "$engine" | cut -d' ' -f1)
    if [ -n "$expected" ] && [ "$found" != "$expected" ]; then
        printf 'play.sh: warning: the engine does not match runtime.json (expected %s, found %s); the game will label this run as unverified.\n' "$expected" "$found" >&2
        unverified=--eawr-engine-unverified
    fi
fi

# (b) the game folder
valid_root() { [ -d "$1/GameData/Data" ] && [ -d "$1/corruption/Data" ]; }
tried=
try_root() {
    [ -n "$1" ] || return 1
    tried="$tried
  $1"
    if valid_root "$1"; then game_root=$1; return 0; fi
    # a folder pointing straight at corruption/
    if [ "$(basename -- "$1")" = corruption ] && valid_root "$(dirname -- "$1")"; then game_root=$(dirname -- "$1"); return 0; fi
    return 1
}
found_root=0
if [ -n "$game_root" ]; then
    try_root "$game_root" && found_root=1
else
    for steam in "$HOME/.steam/steam" "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -d "$steam" ] || continue
        try_root "$steam/steamapps/common/Star Wars Empire at War" && { found_root=1; break; }
        vdf="$steam/steamapps/libraryfolders.vdf"
        if [ -f "$vdf" ]; then
            libs=$(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf")
            old_ifs=$IFS; IFS='
'
            set -f
            for lib in $libs; do
                if try_root "$lib/steamapps/common/Star Wars Empire at War"; then found_root=1; break; fi
            done
            set +f
            IFS=$old_ifs
            [ "$found_root" = 1 ] && break
        fi
    done
fi
if [ "$found_root" != 1 ]; then
    fail "cannot find your Empire at War: Forces of Corruption folder (it must contain GameData and corruption). Looked in:${tried:-
  (no Steam library found)}
Pass it with: ./play.sh --game-root /path/to/\"Star Wars Empire at War\"" 3
fi

if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary >/dev/null 2>&1 || printf 'play.sh: warning: vulkaninfo reports no working Vulkan driver; install your GPU vendor Vulkan driver (mesa-vulkan-drivers, or the NVIDIA/AMD driver package). mesa-vulkan-drivers also gives software rendering (lavapipe).\n' >&2
fi

# the engine runs from another working folder: hand it an absolute path
case $game_root in /*) ;; *) game_root=$(CDPATH= cd -- "$game_root" && pwd) ;; esac

if [ "$check_only" = 1 ]; then
    engine_status=verified
    [ -z "$unverified" ] || engine_status=unverified
    printf 'play.sh: checks passed (glibc %s, engine %s, game folder %s)\n' "${glibc:-unknown}" "$engine_status" "$game_root"
    exit 0
fi

state=${XDG_STATE_HOME:-$HOME/.local/state}/eaw-revised
mkdir -p "$state" 2>/dev/null || state=${TMPDIR:-/tmp}
report="$state/last-run.json"

# shellcheck disable=SC2086
exec "$engine" --path "$project" --rendering-driver vulkan -- --eawr-game-root "$game_root" --eawr-front-end \
    --eawr-launcher-start $unverified --eawr-report "$report" "$@"
