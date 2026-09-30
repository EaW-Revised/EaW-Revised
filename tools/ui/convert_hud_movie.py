"""Convert FoC HUD movies into the local Theora cache the viewer plays (#237).

FoC's HUD movies are Bink 1, which Godot cannot decode. This tool runs on the
player's machine with the player's own FFmpeg: the project neither ships nor
downloads FFmpeg or any Bink decoder, and the cache (derived from the player's
install) stays on that machine. See docs/ui/hud-movies.md.

    python tools/ui/convert_hud_movie.py --prepare <hud_movie_prepare> \
        --game-root <FoC install> [--mod-root "leaf;parent"] --cache <dir> \
        --name Underworld_soldier_Loop [--name ...] [--ffmpeg <ffmpeg>]

FFmpeg comes from --ffmpeg, else EAWR_FFMPEG, else PATH. hud_movie_prepare
(a root build target) resolves each name through the install's VFS and copies
the Bink bytes into the cache; this script converts and verifies them, then
deletes the copy. Entries already in the cache are kept.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

CONVERTER_MISSING = "EAWR-UI-0706"
CONVERSION_FAILED = "EAWR-UI-0707"
KEY = re.compile(r"[0-9a-f]{64}-hud-v1-(alpha|rgb)")

# Theora has no alpha channel. An Alpha movie is packed as colour in the left
# half and opacity (grey) in the right half of one frame, so the two can never
# drift apart; the viewer's shader restores straight alpha. format=rgba gives
# an Alpha movie without an alpha plane an opaque one.
ALPHA_PACKING = ("[0:v]format=rgba,split[colour][opacity];[colour]format=rgb24[c];"
                 "[opacity]alphaextract,format=rgb24[a];[c][a]hstack[v]")


class ConversionError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}" if code else message)
        self.code = code


def find_ffmpeg(explicit: str | None) -> str:
    """The player's FFmpeg, checked for the Bink decoder and the Theora encoder."""
    candidate = explicit or os.environ.get("EAWR_FFMPEG") or shutil.which("ffmpeg")
    if not candidate:
        raise ConversionError(CONVERTER_MISSING, "no FFmpeg found; install FFmpeg (with libtheora) and pass "
                              "--ffmpeg, set EAWR_FFMPEG or put ffmpeg on PATH")
    try:
        decoders = subprocess.run([candidate, "-hide_banner", "-decoders"], capture_output=True, text=True,
                                  check=True, timeout=60).stdout
        encoders = subprocess.run([candidate, "-hide_banner", "-encoders"], capture_output=True, text=True,
                                  check=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError) as error:
        raise ConversionError(CONVERTER_MISSING, f"FFmpeg at {candidate} does not run: {error}") from error
    if not re.search(r"^\s*V\S*\s+binkvideo\b", decoders, re.MULTILINE):
        raise ConversionError(CONVERTER_MISSING, f"FFmpeg at {candidate} has no Bink video decoder")
    if not re.search(r"^\s*V\S*\s+libtheora\b", encoders, re.MULTILINE):
        raise ConversionError(CONVERTER_MISSING, f"FFmpeg at {candidate} has no libtheora encoder")
    return candidate


def convert(source: Path, output: Path, ffmpeg: str, alpha: bool) -> None:
    """Encode `source` to Theora at `output`; an existing `output` survives any failure."""
    command = [ffmpeg, "-nostdin", "-hide_banner", "-v", "error", "-threads", "1", "-i", str(source)]
    if alpha:
        command += ["-filter_complex_threads", "1", "-filter_complex", ALPHA_PACKING, "-map", "[v]"]
    else:
        command += ["-map", "0:v:0"]
    # HUD dialogue and sound come from the speech and SFX systems, not the movie.
    command += ["-an", "-c:v", "libtheora", "-q:v", "10", "-pix_fmt", "yuv420p", "-threads", "1"]
    output.parent.mkdir(parents=True, exist_ok=True)
    handle, name = tempfile.mkstemp(suffix=".ogv.part", dir=output.parent)
    os.close(handle)
    temporary = Path(name)
    try:
        subprocess.run(command + ["-f", "ogg", "-y", str(temporary)], check=True, capture_output=True, text=True,
                       timeout=600)
        with open(temporary, "rb") as stream:
            if stream.read(4) != b"OggS":
                raise ConversionError(CONVERSION_FAILED, f"FFmpeg wrote no Ogg stream for {source.name}")
        # Decode the whole result once, so a truncated file never enters the cache.
        subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-v", "error", "-xerror", "-i", str(temporary),
                        "-map", "0:v:0", "-f", "null", "-"], check=True, capture_output=True, text=True, timeout=600)
        os.replace(temporary, output)
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or "").strip().splitlines()
        raise ConversionError(CONVERSION_FAILED, f"FFmpeg failed on {source.name}: "
                              + (detail[-1] if detail else f"exit {error.returncode}")) from error
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ConversionError(CONVERSION_FAILED, f"converting {source.name} failed: {error}") from error
    finally:
        temporary.unlink(missing_ok=True)


def prepare(args: argparse.Namespace) -> list[tuple[str, str, str]]:
    """Runs hud_movie_prepare: (name, cache key, cached|extracted) per movie."""
    command = [args.prepare, "--game-root", args.game_root, "--cache", str(args.cache)]
    if args.mod_root:
        command += ["--mod-root", args.mod_root]
    for name in args.name:
        command += ["--name", name]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as error:
        raise ConversionError(CONVERSION_FAILED, f"hud_movie_prepare did not run: {error}") from error
    if result.returncode != 0:
        # Its stderr is the resolution diagnostic (EAWR-UI-0701..0704, VFS).
        raise ConversionError("", (result.stderr or f"hud_movie_prepare exit {result.returncode}").strip())
    rows = []
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 3 or not KEY.fullmatch(parts[1]) or parts[2] not in ("cached", "extracted"):
            raise ConversionError(CONVERSION_FAILED, f"unexpected hud_movie_prepare output: {line!r}")
        rows.append((parts[0], parts[1], parts[2]))
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prepare", required=True, help="the built hud_movie_prepare executable")
    parser.add_argument("--ffmpeg", help="the player's FFmpeg (default: EAWR_FFMPEG, then PATH)")
    parser.add_argument("--game-root", required=True, help="FoC install (corruption/ and GameData/)")
    parser.add_argument("--mod-root", help='mod chain, leaf first: "leaf;parent"')
    parser.add_argument("--name", action="append", required=True, help="movies.xml name; repeatable")
    parser.add_argument("--cache", type=Path, required=True, help="the local movie cache directory")
    args = parser.parse_args(argv)
    try:
        ffmpeg = find_ffmpeg(args.ffmpeg)
        rows = prepare(args)
        try:
            for name, key, state in rows:
                output = args.cache / f"{key}.ogv"
                # Two names can share one file, and so one entry.
                if state == "extracted" and not output.is_file():
                    convert(args.cache / f"{key}.bik", output, ffmpeg, key.endswith("-alpha"))
                print(f"{name}\t{'cached' if state == 'cached' else 'converted'}\t{output}")
        finally:
            for _, key, state in rows:
                if state == "extracted":
                    (args.cache / f"{key}.bik").unlink(missing_ok=True)
        return 0
    except ConversionError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
