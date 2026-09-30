#!/usr/bin/env python3
"""Render one viewer map scene on Linux x64 in software (Xvfb + Mesa lavapipe Vulkan).

CI uses it for the asset-free synthetic populated scene and, on the trusted
project runner, FoC reference maps from a read-only installation. Every root is
an argument, so the script names no host path. Build the Linux extension into
apps/viewer/project/bin first (apps/viewer/README.md).

  python3 apps/viewer/tools/linux_software_render.py --godot <Godot 4.7.2 linux binary> \\
      --out out/linux-software/<run> --name <name> \\
      (--synthetic-scene | --game-root <install root> --map <logical .ted path>) \\
      [--timed-frames <n>] [--vulkan-icd <lavapipe ICD json>] \\
      [-- <extra viewer options, e.g. --eawr-populate>]

The viewer runs Forward+ on the Vulkan driver, with the Vulkan loader pinned to
Mesa's lavapipe ICD (package mesa-vulkan-drivers). Writes <name>.png (capture),
<name>.json (viewer report) and <name>.log (engine output) under --out. Exits 0
only when the viewer reported a successful render, wrote a PNG capture, ran the
requested timed frames, and both the engine banner and the report show a
Vulkan Forward+ llvmpipe device. Godot falls back to OpenGL when Vulkan fails,
so an llvmpipe OpenGL banner is a failure here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
PROJECT = ROOT / "apps/viewer/project"
EXTENSION = PROJECT / "bin/libeawr_viewer.linux.template_release.x86_64.so"
# Engine banner of the RenderingDevice path (servers/rendering/rendering_device.cpp).
VULKAN_BANNER = re.compile(
    r"^Vulkan (?P<api>\S+) - Forward\+ - Using Device #\d+: (?P<vendor>.+?) - (?P<adapter>.+)$")
# Any device banner, including the OpenGL fallback, for the failure message.
DEVICE = re.compile(r"Using Device(?: #\d+)?: (.+)")
# mesa-vulkan-drivers names the lavapipe ICD without the architecture suffix
# on the runner's Mesa 25.2.8 (Ubuntu 24.04); older packages used the suffix.
LAVAPIPE_ICDS = (
    pathlib.Path("/usr/share/vulkan/icd.d/lvp_icd.json"),
    pathlib.Path("/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"),
)
# CI frame budget: lavapipe draws populated Naboo about 5x slower per frame
# than llvmpipe GL did (FP-4, #152). 30 warm-up frames plus 40 timed frames
# stay above the viewer's default 60 particle frames, as map mode requires.
CI_TIMED_FRAMES = 40
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def parse(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    extra: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", required=True, type=pathlib.Path, help="pinned Godot 4.7.2 Linux x86_64 binary")
    parser.add_argument("--out", required=True, type=pathlib.Path, help="output directory (use the ignored out/ tree)")
    parser.add_argument("--name", required=True, help="basename for the capture, report and log")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--synthetic-scene", action="store_true",
                        help="render tests/assets/fixtures/scene_fixture.py from a temporary game root")
    source.add_argument("--game-root", type=pathlib.Path, help="read-only installation root holding GameData/Data")
    parser.add_argument("--map", help="logical map path; required with --game-root")
    parser.add_argument("--no-xvfb", action="store_true", help="use the current DISPLAY instead of xvfb-run")
    parser.add_argument("--timeout", type=int, default=900, help="seconds before the run is killed")
    parser.add_argument("--timed-frames", type=int, default=CI_TIMED_FRAMES,
                        help=f"viewer timed frames after warm-up (default {CI_TIMED_FRAMES}, the CI budget)")
    parser.add_argument("--vulkan-icd", type=pathlib.Path,
                        help="lavapipe ICD manifest; default: the first of " + ", ".join(map(str, LAVAPIPE_ICDS)))
    args = parser.parse_args(argv)
    if args.game_root and not args.map:
        parser.error("--game-root needs --map")
    if args.synthetic_scene and args.map:
        parser.error("--synthetic-scene brings its own map")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.name):
        parser.error("--name must be a plain file basename")
    if args.timed_frames < 1:
        parser.error("--timed-frames must be positive")
    if "--eawr-map-timed-frames" in extra:
        parser.error("use --timed-frames, not --eawr-map-timed-frames")
    return args, extra


def fail(message: str) -> int:
    print(f"linux_software_render: FAIL: {message}", file=sys.stderr)
    return 1


def find_lavapipe_icd(candidates: tuple[pathlib.Path, ...] = LAVAPIPE_ICDS) -> pathlib.Path | None:
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def viewer_command(godot: pathlib.Path, game_root: pathlib.Path, map_path: str, capture: pathlib.Path,
                   report: pathlib.Path, timed_frames: int, extra: list[str], xvfb: bool) -> list[str]:
    command = [str(godot), "--path", str(PROJECT),
               "--rendering-driver", "vulkan", "--display-driver", "x11", "--",
               "--eawr-map", map_path, "--eawr-game-root", str(game_root),
               "--eawr-capture", str(capture), "--eawr-report", str(report),
               "--eawr-map-timed-frames", str(timed_frames), *extra]
    if xvfb:
        command = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", *command]
    return command


def viewer_environment(base: dict[str, str], icd: pathlib.Path, xvfb: bool) -> dict[str, str]:
    """Pin the Vulkan loader to lavapipe; no OpenGL software switches remain."""
    env = dict(base, VK_ICD_FILENAMES=str(icd))
    # VK_DRIVER_FILES would take precedence over VK_ICD_FILENAMES.
    for name in ("LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER", "VK_DRIVER_FILES", "WAYLAND_DISPLAY"):
        env.pop(name, None)
    if xvfb:
        env.pop("DISPLAY", None)
    return env


def check_software_vulkan(engine_output: str, result: dict, timed_frames: int) -> str | None:
    """Return why this run was not a lavapipe Forward+ render, or None."""
    banners = [match for match in map(VULKAN_BANNER.match, map(str.strip, engine_output.splitlines())) if match]
    if len(banners) != 1:
        devices = DEVICE.findall(engine_output)
        return (f"expected one Vulkan Forward+ device banner, found {len(banners)} "
                f"(device {devices[-1] if devices else 'unreported'})")
    banner = banners[0]
    if "llvmpipe" not in banner["adapter"]:
        return f"the Vulkan device {banner['adapter']!r} is not lavapipe (llvmpipe), so this was not a software render"
    backend = result.get("backend") if isinstance(result.get("backend"), dict) else {}
    expected = {"rendering_method": "forward_plus", "driver_api": banner["api"],
                "adapter_vendor": banner["vendor"], "adapter_name": banner["adapter"]}
    for key, value in expected.items():
        if backend.get(key) != value:
            return f"report backend.{key} is {backend.get(key)!r}; the engine banner says {value!r}"
    frame_time = result.get("frame_time") if isinstance(result.get("frame_time"), dict) else {}
    if frame_time.get("timed_frames") != timed_frames:
        return f"report frame_time.timed_frames is {frame_time.get('timed_frames')!r}, requested {timed_frames}"
    return None


def render(args: argparse.Namespace, extra: list[str], game_root: pathlib.Path, map_path: str,
           icd: pathlib.Path) -> int:
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    capture, report, log = (out / f"{args.name}{suffix}" for suffix in (".png", ".json", ".log"))
    # A stale capture or report from an earlier run must never pass this one.
    for stale in out.glob(f"{args.name}.*"):
        stale.unlink()

    xvfb = not args.no_xvfb
    command = viewer_command(args.godot.resolve(), game_root, map_path, capture, report,
                             args.timed_frames, extra, xvfb)
    env = viewer_environment(dict(os.environ), icd, xvfb)

    print(f"linux_software_render: VK_ICD_FILENAMES={icd}", flush=True)

    print("linux_software_render: " + " ".join(command), flush=True)
    started = time.monotonic()
    # Own session, so a timeout takes down xvfb-run, Xvfb and Godot together.
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=args.timeout)
        code = process.returncode
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        code = None
    elapsed = time.monotonic() - started
    log.write_bytes(output)
    text = output.decode("utf-8", errors="replace")

    devices = DEVICE.findall(text)
    print(f"linux_software_render: exit {code} after {elapsed:.1f} s; device {devices[-1] if devices else 'unreported'}")
    for line in text.splitlines():
        if VULKAN_BANNER.match(line.strip()):
            print(f"linux_software_render: banner {line.strip()}")
    if code is None:
        return fail(f"timed out after {args.timeout} s; see {log}")
    if not report.is_file():
        return fail(f"no report was written (exit {code}); see {log}")
    result = json.loads(report.read_text(encoding="utf-8"))
    status = result.get("status", "")
    print(f"linux_software_render: status {status}; failure {result.get('failure') or '-'}")
    frame_time = result.get("frame_time") if isinstance(result.get("frame_time"), dict) else {}
    print(f"linux_software_render: {frame_time.get('warmup_frames')} warm-up + {frame_time.get('timed_frames')} "
          f"timed frames at {frame_time.get('milliseconds_per_frame')} ms/frame")
    # Space map mode reports its successful environment pass with this status;
    # land and synthetic map modes use *_passed.
    passed = status.endswith("_passed") or (
        result.get("map", {}).get("kind") == "space" and status == "space_environment_rendered"
    )
    if code != 0 or not passed or result.get("failure"):
        return fail(f"viewer did not pass (exit {code}, status {status!r}); see {report}")
    if (problem := check_software_vulkan(text, result, args.timed_frames)) is not None:
        return fail(problem)
    if not capture.is_file() or capture.read_bytes()[:8] != PNG_SIGNATURE:
        return fail(f"no PNG capture at {capture}")
    print(f"linux_software_render: capture {capture.name} sha256 {hashlib.sha256(capture.read_bytes()).hexdigest()}")
    return 0


def check_synthetic(scene_fixture, report: pathlib.Path, populated: bool) -> int:
    result = json.loads(report.read_text(encoding="utf-8"))
    if not result.get("evidence", {}).get("verified"):
        return fail("the synthetic terrain footprint evidence is not verified")
    if populated:
        populate = result.get("populate", {})
        expected = scene_fixture.EXPECTED["drawable"]
        if populate.get("drawn") != expected or not populate.get("evidence", {}).get("verified"):
            return fail(f"the synthetic scene drew {populate.get('drawn')} of {expected} units without verified coverage")
        print(f"linux_software_render: synthetic scene drew {expected} units; scene_sha256 {populate.get('scene_sha256')}")
    return 0


def main(argv: list[str]) -> int:
    args, extra = parse(argv)
    if not sys.platform.startswith("linux") or platform.machine() != "x86_64":
        return fail(f"this is the Linux x86_64 software path; host is {sys.platform}/{platform.machine()}")
    if not args.godot.is_file():
        return fail(f"no Godot binary at {args.godot}")
    if not EXTENSION.is_file():
        return fail(f"build the Linux viewer extension first; {EXTENSION.relative_to(ROOT)} is missing")
    if not args.no_xvfb and shutil.which("xvfb-run") is None:
        return fail("xvfb-run is not installed (Debian/Ubuntu package xvfb)")
    icd = args.vulkan_icd.resolve() if args.vulkan_icd else find_lavapipe_icd()
    if icd is None or not icd.is_file():
        where = args.vulkan_icd or " or ".join(map(str, LAVAPIPE_ICDS))
        return fail(f"no lavapipe Vulkan ICD at {where} (Debian/Ubuntu packages mesa-vulkan-drivers libvulkan1)")

    if not args.synthetic_scene:
        return render(args, extra, args.game_root.resolve(), args.map, icd)
    sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
    import scene_fixture  # noqa: E402

    with tempfile.TemporaryDirectory(prefix="eawr-software-render-") as temporary:
        root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
        code = render(args, extra, root, scene_fixture.MAP_LOGICAL_PATH, icd)
    if code != 0:
        return code
    return check_synthetic(scene_fixture, args.out.resolve() / f"{args.name}.json", "--eawr-populate" in extra)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
