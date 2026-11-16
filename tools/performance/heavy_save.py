#!/usr/bin/env python3
"""Re-record and check the heavy battle saves of the PF measurements (remake_bench scenes of the `heavy-save` group).

The owner's heavy saves (Remake 4.0 + FFT Anaxes Sprawl, RaW Yavin) are refused by every build whose unit-table content
identity differs from the recording build's, and old saves are not kept across such changes before 1.0. This tool makes
the same scenes again with the current build from a scripted skirmish (the scene's arguments in remake_bench.py: map,
seed, slots, AI levels, credits) and writes the battle save at the owner's tick:

    python tools/performance/heavy_save.py record --scene heavy-anaxes --viewer-artifact <viewer-artifact.json> \
        --machine <gpu host> --mod-root "<leaf-first chain>" --out out/pf/saves
    python tools/performance/heavy_save.py check --save out/pf/saves/Heavy.eawrsave --scene heavy-anaxes \
        --viewer-artifact <viewer-artifact.json> --machine <gpu host> --mod-root "<chain>"
    python tools/performance/heavy_save.py describe <save>        # setup and command count of any save (read only)

`record` runs the scene to the save tick on a rig host (tools/rig/Invoke-RigViewer.ps1), copies the save back and
describes it. `check` loads the save from the front end's Load Game list (the way the owner's saves are loaded) and
plays WINDOW ticks after it, writing the per-frame trace; compare its frame times with an old save's on the same host.
Both only write below --out. The saves are game data derived files: they stay in the ignored out/ folder.
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import shutil
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "common"))
import hidden_console  # noqa: E402  (the rig and git children of a detached run open no console window)

hidden_console.install()
# The checkout the viewer artifact was built from (a copy of this tool may run from an ignored folder inside it, so that the
# rig's "worktree files equal the artifact's commit" check sees no changed tool files).
REPO = pathlib.Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=HERE, text=True).strip())
sys.path.insert(0, str(REPO / "tools" / "performance"))
sys.path.insert(0, str(HERE))  # a copy of remake_bench.py beside this tool wins
import remake_bench  # noqa: E402  (the scenes)

SAVE_TICKS = {"heavy-anaxes": 20310, "heavy-anaxes-ai": 20310, "raw-heavy-yavin": 19700}
WINDOW = 1100        # ticks `check` plays after the load
SAVE_MARGIN = 30     # the saving run plays this far past the save tick


def describe(path: pathlib.Path) -> dict:
    """The setup of a battle save from its embedded replay (section 10): header, SKSU slots, command counts."""
    data = path.read_bytes()
    if data[:7] != b"EAWRCKP":
        raise SystemExit(f"{path}: not a checkpoint")
    seed, tick = struct.unpack_from("<QQ", data, 184)
    section_count = struct.unpack_from("<I", data, 220)[0]
    position, sections = 304, {}
    for _ in range(section_count):
        section_id, _version, _flags, _reserved, length = struct.unpack_from("<HHHHQ", data, position)
        sections[section_id] = (position + 16, length)
        position += 16 + length
    offset, length = sections[10]
    child = data[offset:offset + length]
    conflict = struct.unpack_from("<I", child, 40)[0]
    replay_length = struct.unpack_from("<Q", child, 44 + conflict)[0]
    replay = child[52 + conflict:52 + conflict + replay_length]
    header_size = struct.unpack_from("<H", replay, 10)[0]
    players, squads = struct.unpack_from("<II", replay, 48)
    units, commands = struct.unpack_from("<QQ", replay, 56)
    out = dict(file=path.name, bytes=len(data), seed=seed, tick=tick, players=players, setup_units=units, commands=commands)
    tags, cursor = {}, 104
    for _ in range(struct.unpack_from("<I", replay, cursor)[0]):
        tag, size = struct.unpack_from("<HH", replay, cursor + 4)
        tags[tag] = replay[cursor + 8:cursor + 8 + size]
        cursor += 4 + size
    if 2 in tags:
        body, q = tags[2], 0

        def string():
            nonlocal q
            size = struct.unpack_from("<H", body, q)[0]
            q += 2
            value = body[q:q + size].decode()
            q += size
            return value

        out["map"], _digest = string(), string()
        q += 4
        values = struct.unpack_from("<7q", body, q)
        q += 56
        out["credits"], out["start_tech"], out["max_tech"] = values[0] / (1 << 24), values[1], values[2]
        out["tactical_win"] = (string(), string())[1]
        q += 4  # the lobby victory condition
        slot_count = struct.unpack_from("<I", body, q)[0]
        q += 4
        slots = []
        for _ in range(slot_count):
            player, control, palette, fleet = struct.unpack_from("<4I", body, q)
            q += 16
            for _f in range(fleet):
                string()
            slots.append(dict(player=player, human=bool(control & 1), ai_level=("normal", "easy", "hard")[(control >> 1) & 3],
                              palette=palette, fleet=fleet))
        out["slots"] = slots
    cursor = header_size
    table = []
    for _ in range(players):
        player, team, faction, flags = struct.unpack_from("<IIQI", replay, cursor)
        cursor += 24
        table.append(dict(player=player, team=team, faction=faction, commandable=bool(flags & 1)))
    out["player_table"] = table
    cursor += 80 * units
    for _ in range(squads):
        _container, craft = struct.unpack_from("<QI", replay, cursor)
        cursor += 16 + 8 * craft
    by_player = collections.Counter()
    for _ in range(commands):
        size = struct.unpack_from("<I", replay, cursor)[0]
        by_player[struct.unpack_from("<I", replay, cursor + 12)[0]] += 1
        cursor += 4 + size
    out["commands_by_player"] = dict(sorted(by_player.items()))
    return out


def scene_run_args(scene: str, mod_root: str | None, raw_mod_root: str | None) -> list[str]:
    spec = remake_bench.SCENES[scene]
    args = list(spec["args"])
    if remake_bench.REMAKE_CHAIN in args and not mod_root:
        raise SystemExit("this scene needs --mod-root (leaf-first chain on the GPU host)")
    if remake_bench.RAW_CHAIN in args and not raw_mod_root:
        raise SystemExit("this scene needs --raw-mod-root")
    return [mod_root if a == remake_bench.REMAKE_CHAIN else raw_mod_root if a == remake_bench.RAW_CHAIN else a for a in args]


def write_settings(folder: pathlib.Path) -> pathlib.Path:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "uncapped.txt").write_text("eawr-settings 1\nvideo.vsync=0\nvideo.frame_rate_limit=0\n", encoding="utf-8")
    return folder


def run_rig(args: argparse.Namespace, spec: pathlib.Path, out: pathlib.Path) -> int:
    command = ["pwsh", "-NoProfile", "-File", str(REPO / "tools/rig/Invoke-RigViewer.ps1"), "-Spec", str(spec),
               "-Resolution", "1280x720", "-TimeoutSeconds", str(args.timeout), "-QueueTimeoutSeconds", str(args.queue_timeout),
               "-OutRoot", str(out), "-KeepEvidence"]
    for machine in args.machine:
        command += ["-Machine", machine]
    if args.viewer_artifact:
        command += ["-ViewerArtifact", args.viewer_artifact]
    print(" ".join(command), flush=True)
    return subprocess.call(command, cwd=REPO)


def record(args: argparse.Namespace) -> int:
    scene = args.scene
    tick = args.save_tick or SAVE_TICKS[scene]
    out = pathlib.Path(args.out).resolve()
    work = out / f"record-{scene}"
    work.mkdir(parents=True, exist_ok=True)
    settings = write_settings(work / "settings")
    run_args = scene_run_args(scene, args.mod_root, args.raw_mod_root)
    for flag in ("--eawr-live-capture-ticks",):
        if flag in run_args:
            index = run_args.index(flag)
            del run_args[index:index + 2]
    run_args[run_args.index("--eawr-live-ticks") + 1] = str(tick + SAVE_MARGIN)
    run_args += ["--eawr-live-save-at", f"{tick}:" + "{out}/" + f"{scene}.eawrsave",
                 "--eawr-live-audio-pace", "off", "--eawr-settings-file", "{in}/settings/uncapped.txt",
                 "--eawr-diagnostic-vsync", "0", "--eawr-diagnostic-max-fps", "0",
                 "--eawr-perf-trace", "{out}/" + f"{scene}.csv", "--eawr-save-dir", "{out}/saves"]
    spec = dict(inputs=dict(settings=str(settings)),
                runs=[dict(name=scene, args=run_args, capture=False, engineArgs=["--max-fps", "0", "--render-thread", "separate"],
                           logMode="pipe")])
    spec_path = work / "spec.json"
    spec_path.write_text(json.dumps(spec, indent=2), encoding="utf-8")
    if args.plan_only:
        print(spec_path)
        return 0
    code = run_rig(args, spec_path, work / "rig")
    saves = sorted((work / "rig").rglob(f"{scene}.eawrsave"))
    if code != 0 or not saves:
        print(f"record failed: rig exit {code}, save {'missing' if not saves else saves[-1]}", file=sys.stderr)
        return 1
    target = out / args.name
    shutil.copyfile(saves[-1], target)
    summary = describe(target)
    (target.with_suffix(".setup.json")).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=1))
    return 0


def profile_json(mod_root: str) -> str:
    layers = []
    for root in mod_root.split(";"):
        root = root.strip()
        item = root.replace("\\", "/").rstrip("/").split("/")[-1]
        layers.append(dict(key=f"workshop:{item}", root=root.replace("/", "\\")))
    return json.dumps(dict(format="eawr-mod-profile", version=1, id="profile-heavy-save-check", name="heavy save check",
                           layers=layers, order_source="player", separators=[],
                           launch=dict(mode="front-end", render_thread="separate", frame_rate_limit=None),
                           accepted=dict(chain_fingerprint="", load_check="")), separators=(",", ":"))


def check(args: argparse.Namespace) -> int:
    save = pathlib.Path(args.save).resolve()
    out = pathlib.Path(args.out).resolve()
    work = out / f"check-{args.scene}"
    saves, profile, observer = work / "saves", work / "profile", work / "observer"
    for folder in (saves, profile, observer):
        folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(save, saves / "Heavy.eawrsave")
    chain = args.raw_mod_root if remake_bench.RAW_CHAIN in remake_bench.SCENES[args.scene]["args"] else args.mod_root
    (profile / "profile.json").write_text(profile_json(chain), encoding="utf-8")
    shutil.copyfile(HERE / "heavy_save_observer.gd", observer / "load.gd")
    settings = write_settings(work / "settings")
    runs = []
    for label in args.labels:
        runs.append(dict(name=label, args=["--eawr-front-end", "--eawr-audio", "off", "--eawr-save-dir", "{in}/saves",
                                           "--eawr-diagnostic-vsync", "0", "--eawr-diagnostic-max-fps", "0",
                                           "--eawr-perf-trace", "{out}/" + label + ".csv", "--bench-output={out}",
                                           "--bench-label=" + label, f"--bench-ticks={WINDOW}", "--eawr-mod-profile",
                                           "{in}/profile/profile.json", "--eawr-perf-no-render-timing",
                                           "--eawr-settings-file", "{in}/settings/uncapped.txt"],
                         capture=False, engineArgs=["--max-fps", "0", "--render-thread", "separate", "--script", "{in}/observer/load.gd"],
                         logMode="pipe"))
    spec = dict(inputs=dict(saves=str(saves), profile=str(profile), observer=str(observer), settings=str(settings)), runs=runs)
    spec_path = work / "spec.json"
    spec_path.write_text(json.dumps(spec, indent=2), encoding="utf-8")
    if args.plan_only:
        print(spec_path)
        return 0
    return run_rig(args, spec_path, work / "rig")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("record", "check"):
        p = sub.add_parser(name)
        p.add_argument("--scene", required=True, choices=sorted(SAVE_TICKS))
        p.add_argument("--viewer-artifact")
        p.add_argument("--machine", action="append", default=[])
        p.add_argument("--mod-root", help="leaf-first Remake chain on the GPU host")
        p.add_argument("--raw-mod-root", help="RaW chain on the GPU host")
        p.add_argument("--out", default="out/pf/saves")
        p.add_argument("--timeout", type=int, default=5400)
        p.add_argument("--queue-timeout", type=int, default=7200)
        p.add_argument("--plan-only", action="store_true")
        if name == "record":
            p.add_argument("--name", default="Heavy.eawrsave", help="file name of the copied-back save")
            p.add_argument("--save-tick", type=int, help="save at this tick instead of the scene's (the owner's) tick")
        else:
            p.add_argument("--save", required=True)
            p.add_argument("--labels", nargs="+", default=["load1"])
    d = sub.add_parser("describe")
    d.add_argument("save", type=pathlib.Path)
    parsed = parser.parse_args()
    if parsed.command == "describe":
        print(json.dumps(describe(parsed.save), indent=1))
        return 0
    return record(parsed) if parsed.command == "record" else check(parsed)


if __name__ == "__main__":
    sys.exit(main())
