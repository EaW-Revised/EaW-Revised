#!/usr/bin/env python3
"""Repeatable Remake performance harness (perf wave #4645, P3).

One command per job:

    python tools/performance/remake_bench.py run --viewer-artifact <viewer-artifact.json> \
        --machine <gpu-host> --mod-root <leaf-first-chain> [--scenes smoke|full|<name,name>]

It writes a rig run spec (one viewer process per scene, fixed seed, no input, the proven pacing flags),
runs it hidden on a private desktop through tools/rig/Invoke-RigViewer.ps1 and then analyses the copied
back per-frame CSVs into results.json / results.md: one row per scene with the one-number result
(median and p95 wall frame ms) plus the controls (viewer main-thread ms, render CPU/GPU ms, draws,
units, private GiB, first-frame seconds). `analyse <rig-run-dir>` re-reads a finished run; `plan` only
writes the spec. The same host must be used for any before/after comparison.

Scenes (see SCENES): stock control, Remake menu backdrop (attract), the real Korriban map,
provisional additive Devastator fleets (1/2/4 against fixed targets), a fighter-heavy additive
fleet and one additive scene per ship type. Default forces/stations remain present; these
are not isolated ship costs or reproductions of the owner's console/Lua spawning method.

Everything here is measurement-only: no game data is copied, mod folders are read through the viewer's
--eawr-mod-root exactly as the owner's game mounts them.
"""
from __future__ import annotations

import argparse
from collections import Counter
import csv
import hashlib
import json
import math
import pathlib
import os
import re
import statistics
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

REMAKE_CHAIN = "{mod_root}"
RAW_CHAIN = "{raw_mod_root}"
SEED = 4645
# Sim ticks a scene plays (30 ticks per second) and the tick window its numbers come from: after the ships
# have closed and fire, before the fight is decided. Frames outside the window are warm-up or tail.
TICKS = 2400
WINDOW = (600, 2300)
FIGHT = ("Devastator_Star_Destroyer",)
TARGETS = ("Nebulon_B_Frigate", "Nebulon_B_Frigate", "Nebulon_B_Frigate", "X-Wing_Squadron", "Y-Wing_Squadron")
FIGHTERS_EMPIRE = ("TIE_Fighter_Squadron",) * 6 + ("TIE_Interceptor_Squadron",) * 2
FIGHTERS_REBEL = ("X-Wing_Squadron",) * 6 + ("Y-Wing_Squadron",) * 2
# Solo scenes: one ship of the type (Empire, slot 2) against the same fixed targets (Rebel, slot 1).
SOLO_SHIPS = ("Devastator_Star_Destroyer", "Victory_Destroyer", "Nebulon_B_Frigate", "TIE_Fighter_Squadron")
# Benchmark fixtures, using names/affiliations read from the mounted RaW XML.
RAW_CIS = ("Munificent_Frigate", "Providence_Class_Carrier")
RAW_REPUBLIC = ("Venator", "Rep_Acclamator_Assault_Ship")

# The proven live-session flags of the 4645 runs (sk-4645 run-spec.json): lit/shadows, per-frame CSV, overlay.
LIVE_COMMON = (
    "--eawr-live-session", "skirmish", "--eawr-populate",
    "--eawr-skirmish-seed", str(SEED),
    "--eawr-live-begin", "auto",
    "--eawr-live-ai", "on", "--eawr-live-step", "0.5",
    "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
    "--eawr-perf-overlay", "on",
    "--eawr-live-workers", "4", "--eawr-live-speed", "2", "--eawr-live-verify", "off",
)

# The standalone attract runner has no native per-frame trace. This observer loads
# the unchanged production scene and reads completed viewport frames without input.
MENU_OBSERVER = '''extends SceneTree
var output = ""
var label = ""
var previous = 0
var frame = 0
var rows = PackedStringArray(["frame,wall_frame_ms,process_ms,render_cpu_ms,render_gpu_ms,draw_calls,engine_max_fps,vsync_mode"])
var written = false
var started = Time.get_ticks_usec()
func _initialize():
    root.tree_exiting.connect(write_trace)
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--bench-output="):
            output = arg.substr(15)
        if arg.begins_with("--bench-label="):
            label = arg.substr(14)
    call_deferred("observe")
func observe():
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene
    RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
    while true:
        await RenderingServer.frame_post_draw
        var now = Time.get_ticks_usec()
        if previous == 0:
            var first = FileAccess.open(output.path_join(label + ".observer-startup.json"), FileAccess.WRITE)
            first.store_string(JSON.stringify({"first_presented_s": (now - started) / 1000000.0, "backend": {"adapter_name": RenderingServer.get_video_adapter_name()}}))
            first.close()
        rows.append("%d,%f,%f,%f,%f,%d,%d,%d" % [frame, (now - previous) / 1000.0 if previous else 0, Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0, RenderingServer.viewport_get_measured_render_time_cpu(root.get_viewport_rid()), RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()), Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME), Engine.max_fps, DisplayServer.window_get_vsync_mode()])
        previous = now
        frame += 1
func write_trace():
    if written:
        return
    written = true
    var trace = FileAccess.open(output.path_join(label + ".observer.csv"), FileAccess.WRITE)
    trace.store_string("\\n".join(rows) + "\\n")
    trace.close()
'''


def scene_args(name: str, mod: bool, ticks: int, fleets: dict[int, tuple[str, ...]] | None, map_path: str,
               rebel_slot=("1", "Rebel", "0", "human"), empire_slot=("2", "Empire", "1", "ai")) -> list[str]:
    args = list(LIVE_COMMON)
    if mod:
        args += ["--eawr-mod-root", REMAKE_CHAIN]
    args += ["--eawr-skirmish-slot", ":".join(rebel_slot), "--eawr-skirmish-slot", ":".join(empire_slot)]
    for slot, units in (fleets or {}).items():
        args += ["--eawr-skirmish-fleet", f"{slot}:{','.join(units)}"]
    args += ["--eawr-live-ticks", str(ticks), "--eawr-live-capture-ticks", f"0,{ticks}",
             "--eawr-map", map_path]
    return args


def build_scenes() -> dict[str, dict]:
    spec = {}

    def live(name, *, mod, map_path, fleets=None, note, ticks=TICKS, window=WINDOW, tags=(), family=("full",), raw=False):
        args = scene_args(name, mod, ticks, fleets, map_path)
        if raw:
            args = [RAW_CHAIN if a == REMAKE_CHAIN else a for a in args]
        spec[name] = dict(kind="live", mod=mod, mod_kind="raw" if raw else "remake" if mod else "stock",
                          args=args, note=note, fleets=fleets, map=map_path,
                          ticks=ticks, window=window, tags=tuple(family) + tuple(tags))

    live("stock-bespin", mod=False, map_path="data/art/maps/_mp_space_bespin.ted",
         note="stock-content Bespin control: default free forces/stations plus inherited m2 fixture fleets, AI on", tags=("smoke",))
    spec["remake-menu"] = dict(
        kind="attract", mod=True, note="Remake main-menu backdrop battle (--eawr-attract), 3600 frames",
        args=["--eawr-attract", "--eawr-attract-frames", "3600", "--eawr-mod-root", REMAKE_CHAIN],
        ticks=0, window=(600, 3500), tags=("full", "smoke"))
    spec["stock-menu"] = dict(
        kind="attract", mod=False, note="stock FoC main-menu backdrop battle, 3600 frames",
        args=["--eawr-attract", "--eawr-attract-frames", "3600"], ticks=0, window=(600, 3500), tags=("full",))
    for mode in ("file", "pipe", "nul"):
        spec[f"remake-menu-{mode}-control"] = dict(spec["remake-menu"], tags=("logger-control",),
            log_mode=mode, note=f"Remake menu logger {mode} A/B control; identical scene/clock protocol")
    live("remake-korriban", mod=True, map_path="data/art/maps/korriban.ted",
         note="real archived korriban.ted/full chain; default forces/stations plus inherited m2 fixture fleets, AI on",
         tags=("smoke",))
    for n in (1, 2, 4):
        live(f"remake-dev{n}", mod=True, map_path="data/art/maps/korriban.ted",
             fleets={1: TARGETS, 2: FIGHT * n},
             note=f"provisional additive fleet: {n} x Devastator vs fixed targets, plus default forces/stations, Korriban",
             tags=("smoke",) if n == 4 else ())
    live("remake-fighters", mod=True, map_path="data/art/maps/korriban.ted",
         fleets={1: FIGHTERS_REBEL, 2: FIGHTERS_EMPIRE},
         note="fighter-heavy additive fleet: 6+2 Rebel vs 6+2 Empire squadrons plus default forces/stations, Korriban")
    for ship in SOLO_SHIPS:
        live(f"solo-{ship.lower()}", mod=True, map_path="data/art/maps/korriban.ted",
             fleets={1: TARGETS, 2: (ship,)}, note=f"one added {ship} vs fixed targets, default forces/stations retained",
             tags=("ship",))
    for size in (15, 30, 60):
        fleets = {1: tuple(RAW_CIS[i % len(RAW_CIS)] for i in range(size)),
                  2: tuple(RAW_REPUBLIC[i % len(RAW_REPUBLIC)] for i in range(size))}
        live(f"raw-late-fleet{size}", mod=True, raw=True,
             map_path="data/art/maps/_mp_space_coruscant.ted", fleets=fleets,
             ticks=3600, window=(1200, 3500), family=("raw-late-fleet",),
             note=f"RaW: {size} added ships/side (CIS/Rebel vs Republic/Empire), default forces/stations retained; AI on")
    for source in ("raw-late-fleet60", "remake-dev4"):
        for mode in ("file", "pipe", "nul"):
            control = dict(spec[source], tags=("logger-live-control",), log_mode=mode,
                           prepare_source=source, note=f"{spec[source]['note']}; logger {mode} A/B/C control")
            if source == "raw-late-fleet60":
                # Bounded, common tick window authorized after the long file
                # control demonstrated multi-second frames before tick 500.
                control["args"] = list(control["args"])
                control["args"][control["args"].index("--eawr-live-ticks") + 1] = "750"
                capture = control["args"].index("--eawr-live-capture-ticks") + 1
                control["args"][capture] = "0,750"
                control.update(ticks=750, window=(500, 700), fixed_window=True,
                               note=control["note"] + "; bounded early-combat fleet60, not late-match steady state")
            spec[f"{source}-{mode}-control"] = control
    return spec


SCENES = build_scenes()


def start_identity(report: dict) -> str:
    live = report.get("live_session", {})
    start = {key: live.get(key) for key in ("start_map", "start_map_sha256", "start_seed", "start_fleet", "start_markers")}
    return hashlib.sha256(json.dumps(start, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def order_rejections(live: dict, recipe: dict) -> dict:
    """Separate fixture candidates from later AI commands, retaining unknown provenance."""
    owners = {int(r["entity"]): int(r["player"]) for r in live.get("start_fleet", [])}
    commands = []
    for text in recipe.get("orders", []):
        match = re.match(r"(\d+):[^:]+:(\d+)", text)
        if match:
            commands.append((int(match[1]), owners.get(int(match[2]))))
    if not commands:
        commands = [(int(r["tick"]), owners.get(int(r["unit"]))) for r in live.get("orders", [])]
    if not commands:
        # Legacy preparation metadata predates the retained recipe strings.
        commands = [(1, 1), (1, 2)]
    result = dict(fixture_order_rejections=[], ai_order_rejections=[], unattributed_order_rejections=[])
    for rejection in live.get("rejected", []):
        match = re.search(r"command \(tick (\d+), player (\d+), sequence \d+\):", rejection)
        if not match:
            key = "unattributed_order_rejections"
        elif any(tick == int(match[1]) and (player is None or player == int(match[2]))
                 for tick, player in commands):
            key = "fixture_order_rejections"
        else:
            key = "ai_order_rejections"
        result[key].append(rejection)
    return result


def engagement_recipe(report: dict) -> dict:
    """Derive actor IDs and a fixed meeting point from the native start, never from fixture pins."""
    live = report.get("live_session", {})
    if live.get("start_seed") != SEED or not live.get("start_map_sha256"):
        raise ValueError("preparation report has no matching native seed/map identity")
    fleets = live.get("start_fleet", [])
    groups = {player: sorted({int(r["entity"]) for r in fleets if r["player"] == player}) for player in (1, 2)}
    markers = [r for r in live.get("start_markers", []) if r["use"] == "spawn" and r["player"] in groups]
    if not all(groups.values()) or {r["player"] for r in markers} != {1, 2}:
        raise ValueError("preparation needs both native fleets and their spawn markers")
    center = [statistics.fmean(float(r["position"][axis]) for r in markers) for axis in (0, 1)]
    positions = [r["position"] for r in fleets] + [r["position"] for r in markers]
    if not all(len(p) == 3 and all(math.isfinite(float(v)) for v in p) for p in positions):
        raise ValueError("preparation contains an invalid position")
    point = f"{center[0]:.6f},{center[1]:.6f},0"
    orders = ["1:attack_move:" + "+".join(map(str, groups[player])) + "@" + point for player in (1, 2)]
    return dict(center=center, orders=orders, start_identity=start_identity(report),
                map=live["start_map"], map_sha256=live["start_map_sha256"],
                bounds={f"{end}_{axis}": func(float(p[index]) for p in positions) + margin
                        for index, axis in enumerate(("x", "y"))
                        for end, func, margin in (("min", min, -2000), ("max", max, 2000))})


def write_engagement_camera(path: pathlib.Path, recipe: dict) -> None:
    root = ET.Element("map_camera", schema="eawr-space-map-camera", version="1", provenance="project-authored",
                      map_path=recipe["map"], map_sha256=recipe["map_sha256"])
    ET.SubElement(root, "bounds", **{k: str(v) for k, v in recipe["bounds"].items()},
                  source_id="benchmark-native-start", authority="project-authored")
    ET.SubElement(root, "initial", target_x=str(recipe["center"][0]), target_y=str(recipe["center"][1]),
                  target_height="0", zoom="0.5", yaw_degrees="0")
    ET.SubElement(root, "bindings", path="space-live-camera-bindings.json")
    overrides = ET.SubElement(root, "constant_overrides", schema="eawr-map-camera-overrides", version="1",
                              source_id="benchmark-wide-framing", authority="project-authored")
    ET.SubElement(overrides, "override", tag="Distance_Max", value="12000")
    path.write_text(ET.tostring(root, encoding="unicode"), encoding="utf-8")


def select(names: str) -> list[str]:
    if names in ("smoke", "full", "ship", "raw-late-fleet", "logger-control", "logger-live-control"):
        chosen = [n for n, s in SCENES.items() if names in s["tags"]]
    else:
        chosen = [n.strip() for n in names.split(",") if n.strip()]
    unknown = [n for n in chosen if n not in SCENES]
    if unknown or not chosen:
        raise SystemExit(f"unknown or empty scene selection {names!r}: {unknown}; known: {', '.join(SCENES)}")
    return chosen


def write_spec(path: pathlib.Path, scenes: list[str], pacing: str, mod_root: str | None = None,
               raw_mod_root: str | None = None, preparations: dict | None = None, prepare: bool = False) -> None:
    if any(REMAKE_CHAIN in SCENES[n]["args"] for n in scenes) and not mod_root:
        raise SystemExit("Remake scenes require --mod-root with the explicit leaf-first chain")
    if any(RAW_CHAIN in SCENES[n]["args"] for n in scenes) and not raw_mod_root:
        raise SystemExit("RaW scenes require --raw-mod-root with the explicit mounted chain")
    runs = []
    path.parent.mkdir(parents=True, exist_ok=True)
    cameras = path.parent / "engagement"
    cameras.mkdir(exist_ok=True)
    shutil.copyfile(pathlib.Path(__file__).resolve().parents[2] / "apps/viewer/project/config/space-live-camera-bindings.json",
                    cameras / "space-live-camera-bindings.json")
    for name in scenes:
        scene = SCENES[name]
        args = list(scene["args"])
        args = [mod_root if a == REMAKE_CHAIN else raw_mod_root if a == RAW_CHAIN else a for a in args]
        args += ["--eawr-settings-file", "{out}/" + name + "-settings.cfg",
                 "--eawr-save-dir", "{out}/saves"]
        engine = ["--max-fps", "120"] if pacing == "audio" else ["--max-fps", "0"]
        if pacing == "free":
            args += ["--eawr-diagnostic-vsync", "0", "--eawr-diagnostic-max-fps", "0"]
        recipe = None
        if scene["kind"] == "live":
            if prepare:
                args[args.index("--eawr-live-ticks") + 1] = "1"
                index = args.index("--eawr-live-capture-ticks")
                del args[index:index + 2]
            else:
                if not preparations or name not in preparations:
                    raise ValueError(f"{name}: native preparation report required before measurement")
                recipe = engagement_recipe(preparations[name])
                write_engagement_camera(cameras / f"{name}.xml", recipe)
                args += ["--eawr-map-camera-config", "{in}/engagement/" + name + ".xml"]
                for order in recipe["orders"]:
                    args += ["--eawr-live-order", order]
                args += ["--eawr-perf-trace", "{out}/" + name + ".csv"]
            if pacing == "audio" and not prepare:
                args += ["--eawr-live-audio-pace", "on"]
        if not prepare:
            engine += ["--script", "{in}/observer/menu-observer.gd"]
            args += ["--bench-output={out}", "--bench-label=" + name]
        entry = dict(name=name, args=args, capture=not prepare, engineArgs=engine, logMode=scene.get("log_mode", "pipe"),
                         benchmark=dict(note=scene["note"], seed=SEED, ticks=1 if prepare else scene["ticks"],
                                        window=scene["window"], fleets=scene.get("fleets"),
                                        phase="prepare" if prepare else "measure", pacing=pacing, engagement=recipe,
                                        observer="buffered-post-draw-v2" if not prepare else None,
                                        log_mode=scene.get("log_mode", "pipe"),
                                        log_counter="stream-markers-v3",
                                        camera="fixed native-spawn midpoint" if recipe else "native initial view"))
        entry.update(driver="perf-sample.ps1", driverArgs=dict(intervalMs=2000))
        runs.append(entry)
    observer = path.parent / "observer"
    observer.mkdir(exist_ok=True)
    (observer / "menu-observer.gd").write_text(MENU_OBSERVER, encoding="utf-8")
    path.write_text(json.dumps(dict(runs=runs, inputs=dict(observer=str(observer.resolve()), engagement=str(cameras.resolve()))), indent=2), encoding="utf-8")


def pct(sorted_values: list[float], q: float) -> float | None:
    if not sorted_values:
        return None
    rank = max(1, math.ceil(q * len(sorted_values)))
    return sorted_values[rank - 1]


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def column(rows, key) -> list[float]:
    out = []
    for row in rows:
        try:
            value = float(row[key])
            if math.isfinite(value):
                out.append(value)
        except (KeyError, ValueError):
            pass
    return out


def stats(values: list[float]) -> dict:
    ordered = sorted(values)
    return dict(median=statistics.median(ordered) if ordered else None, p95=pct(ordered, 0.95),
                mean=statistics.fmean(ordered) if ordered else None, n=len(ordered))


def first_frame_seconds(directory: pathlib.Path, name: str) -> float | None:
    path = directory / f"{name}.csv.startup.csv"
    if not path.exists():
        observer = directory / f"{name}.observer-startup.json"
        return json.loads(observer.read_text())["first_presented_s"] if observer.exists() else None
    rows = read_csv(path)
    if rows:
        try:
            return float(rows[0]["total_ms"]) / 1000.0
        except (KeyError, ValueError):
            return None
    return None


def process_stats(directory: pathlib.Path, name: str, first_s: float | None) -> dict:
    path = directory / f"{name}.proc.csv"
    if not path.exists():
        return {}
    rows = read_csv(path)
    if not rows:
        return {}
    private = [float(r["private_bytes"]) / 2**30 for r in rows]
    result = dict(private_gib_peak=max(private), private_gib_final=private[-1])
    # Post-startup samples, not a sample of the narrower native frame/tick window.
    start = (first_s or 0) * 1000
    steady = [p for r, p in zip(rows, private) if float(r["elapsed_ms"]) >= start] or private[len(private) // 2:]
    result["private_gib_median"] = statistics.median(steady)
    log_rows = [r for r in rows if r.get("log_bytes") and float(r["elapsed_ms"]) >= start]
    if len(log_rows) >= 2:
        result["log_growth_post_startup"] = dict(bytes=int(log_rows[-1]["log_bytes"]) - int(log_rows[0]["log_bytes"]),
            elapsed_ms=[float(log_rows[0]["elapsed_ms"]), float(log_rows[-1]["elapsed_ms"])],
            scope="process samples after first frame; raw per-sample log sizes retained")
        elapsed = (float(log_rows[-1]["elapsed_ms"]) - float(log_rows[0]["elapsed_ms"])) / 1000
        result["log_growth_post_startup"]["bytes_per_s"] = result["log_growth_post_startup"]["bytes"] / elapsed if elapsed else None
    # Total CPU of the last 60 s of samples, as cores.
    tail = [r for r in rows if float(r["elapsed_ms"]) >= float(rows[-1]["elapsed_ms"]) - 60000]
    if len(tail) >= 2:
        wall = (float(tail[-1]["elapsed_ms"]) - float(tail[0]["elapsed_ms"])) / 1000
        cpu = float(tail[-1]["cpu_seconds"]) - float(tail[0]["cpu_seconds"])
        if wall > 0:
            result["process_cores_tail"] = cpu / wall
    threads = directory / f"{name}.threads.csv"
    if threads.exists() and len(tail) >= 2:
        per: dict[str, list[tuple[float, float]]] = {}
        t0, t1 = float(tail[0]["elapsed_ms"]), float(tail[-1]["elapsed_ms"])
        for r in read_csv(threads):
            ms = float(r["elapsed_ms"])
            if t0 <= ms <= t1:
                per.setdefault(r["thread_id"], []).append((ms, float(r["cpu_seconds"])))
        busy = sorted(((v[-1][1] - v[0][1]) / ((v[-1][0] - v[0][0]) / 1000), tid)
                      for tid, v in per.items() if len(v) >= 2 and v[-1][0] > v[0][0])
        result["busiest_thread_fraction"] = busy[-1][0] if busy else None
        result["top_threads"] = [dict(thread=tid, cores=round(frac, 3)) for frac, tid in reversed(busy[-5:])]
    return result


def window_log_stats(directory: pathlib.Path, name: str, window: list[dict], *, native_begin_frame: bool = False) -> dict:
    """Bracket the frame window with process samples, using retained clock anchors.

    The startup sidecar is written at the first post-draw; proc.info is written
    when the sampler stopwatch starts. Their retained write times align those
    clocks approximately. Keep the actual bracketing interval visible.
    """
    observer = directory / f"{name}.observer.csv"
    first = directory / f"{name}.observer-startup.json"
    info = directory / f"{name}.proc.info"
    process = directory / f"{name}.proc.csv"
    indices = column(window, "frame")
    if not indices or not all(p.exists() for p in (observer, first, info, process)):
        return {}
    times, intervals = {}, {}
    elapsed = (first.stat().st_mtime - info.stat().st_mtime) * 1000
    for frame in read_csv(observer):
        elapsed += float(frame["wall_frame_ms"])
        times[int(frame["frame"])] = elapsed
        intervals[int(frame["frame"])] = float(frame["wall_frame_ms"])
    low, high = int(min(indices)), int(max(indices))
    # Native wall_frame_ms spans process-entry i-1 -> i, approximately the
    # preceding post-draw interval. The observer's own row spans draw i-1 -> i.
    offset = -1 if native_begin_frame else 0
    start_index, end_index = low + offset - 1, high + offset
    if start_index not in times or end_index not in times:
        return {}
    clock_check = None
    if native_begin_frame:
        errors = [abs(float(r["wall_frame_ms"]) - intervals[int(float(r["frame"])) - 1])
                  for r in window if int(float(r["frame"])) - 1 in intervals and r.get("wall_frame_ms")]
        clock_check = dict(native_to_observer_interval_offset=-1, absolute_difference_ms=stats(errors))
        if not errors or statistics.median(errors) > 2.0:
            return dict(log_window_clock_check=clock_check,
                        log_window_error="native/observer interval alignment exceeds 2 ms median; window counts unavailable")
    rows = [r for r in read_csv(process) if r.get("log_bytes")]
    before = [r for r in rows if float(r["elapsed_ms"]) <= times[start_index]]
    after = [r for r in rows if float(r["elapsed_ms"]) >= times[end_index]]
    if not before or not after:
        return {}
    a, b = before[-1], after[0]
    seconds = (float(b["elapsed_ms"]) - float(a["elapsed_ms"])) / 1000
    growth = int(b["log_bytes"]) - int(a["log_bytes"])
    if seconds <= 0 or growth < 0:
        return {}
    counters = {}
    for key in ("log_error_markers", "shader_limit_errors"):
        valid = all(r.get("log_tracking_valid") == "1" and r.get(key, "").isdigit() for r in (a, b))
        delta = int(b[key]) - int(a[key]) if valid else -1
        counters[key] = delta if delta >= 0 else None
    return dict(log_growth_window=dict(bytes=growth, bytes_per_s=growth / seconds, **counters,
        elapsed_ms=[float(a["elapsed_ms"]), float(b["elapsed_ms"])],
        requested_elapsed_ms=[times[start_index], times[end_index]], frame_indices=[low, high],
        observer_boundary_indices=[start_index, end_index], clock_check=clock_check,
        alignment="approximate observer/proc.info file-write clock anchors and frame indices; enclosing process samples"))


def analyse_scene(directory: pathlib.Path, name: str, scene: dict) -> dict:
    row: dict = dict(scene=name, note=scene["note"])
    definition = {}
    log_path = directory / f"{name}.log"
    if log_path.exists():
        contents = log_path.read_bytes()
        row["log_bytes"] = len(contents)
        row["error_markers_total"] = contents.count(b"ERROR:")
        row["shader_instance_limit_errors"] = contents.count(b"Too many instances using shader instance variables")
    run_spec = directory / "_spec.json"
    if run_spec.exists():
        definition = next((r.get("benchmark", {}) for r in json.loads(run_spec.read_text(encoding="utf-8-sig"))["runs"]
                           if r["name"] == name), {})
    row["recorded_protocol_window"] = definition.get("window")
    row["analysis_window_post_hoc"] = bool(definition.get("window") and
                                           list(definition["window"]) != list(scene["window"]))
    exit_codes = directory / "exit-codes.txt"
    if exit_codes.exists():
        for line in exit_codes.read_text().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == name:
                row["exit_code"] = int(parts[1])
    first = first_frame_seconds(directory, name)
    row["first_frame_s"] = first
    row["first_frame_method"] = "native startup trace" if scene["kind"] == "live" else "observer initialization to first post-draw"
    csv_path = directory / (f"{name}.observer.csv" if scene["kind"] == "attract" else f"{name}.csv")
    if csv_path.exists():
        frames = read_csv(csv_path)
        low, high = scene["window"]
        coordinate = "frame" if scene["kind"] == "attract" else "tick"
        if scene["kind"] == "live":
            contact = [int(float(f.get("tick", 0))) for f in frames if float(f.get("projectiles", 0) or 0) > 0]
            row["first_projectile_tick"] = min(contact) if contact else None
            if contact and not scene.get("fixed_window"):
                low = max(low, min(contact) + 60)
        window = [f for f in frames if low <= int(float(f.get(coordinate, 0) or 0)) <= high]
        row["window_coordinate"] = coordinate
        row["window_ticks"] = [low, high]
        row["frames_total"] = len(frames)
        row["frames_window"] = len(window)
        row["window_complete"] = any(int(float(f.get(coordinate, 0) or 0)) >= high for f in frames)
        if window:
            for key, label in (("wall_frame_ms", "frame_ms"), ("viewer_main_ms", "main_ms"),
                               ("process_ms", "process_ms"), ("render_cpu_ms", "render_cpu_ms"),
                               ("render_gpu_ms", "gpu_ms"), ("draw_calls", "draws"), ("units", "units"),
                               ("tick_ms", "tick_ms"), ("pose_ms", "pose_ms"), ("bookkeeping_ms", "bookkeeping_ms"),
                               ("effects", "effects"), ("effect_particles", "effect_particles"), ("projectiles", "projectiles")):
                row[label] = stats(column(window, key))
            projectiles = column(window, "projectiles")
            row["combat_projectiles_observed"] = max(projectiles, default=0) > 0
            row["engine_max_fps_values"] = sorted(set(column(window, "engine_max_fps")))
            row["vsync_mode_values"] = sorted(set(column(window, "vsync_mode")))
        row["result_p95_frame_ms"] = row.get("frame_ms", {}).get("p95")
        row["result_median_frame_ms"] = row.get("frame_ms", {}).get("median")
        median = row["result_median_frame_ms"]
        row["fps_from_median_frame"] = 1000 / median if median else None
    else:
        report = directory / f"{name}.json"
        if report.exists():
            data = json.loads(report.read_text(encoding="utf-8"))
            attract = data.get("attract", data)
            for key in ("frame_ms_mean", "frame_ms_max", "frame_samples", "units_max"):
                if key in attract:
                    row[key] = attract[key]
            row["measurement_error"] = "aggregate mean/max cannot supply median or p95"
    if (not row.get("frames_window") or not row.get("window_complete") or row.get("exit_code") != 0
            or not row.get("result_median_frame_ms") or not row.get("result_p95_frame_ms")):
        row["measurement_error"] = "missing measurement window or nonzero/missing scene exit"
    if scene["kind"] == "live" and not row.get("combat_projectiles_observed"):
        row["measurement_error"] = "fleet engagement not demonstrated by projectiles in the measurement window"
    if definition.get("pacing") == "free":
        # MapMode resets the settings cap at process entry, before its native
        # trace. ViewerHost's existing frame_pre_draw override then sets zero.
        # Observe after drawing, when Godot's limiter actually sees that value.
        clocks_path = directory / f"{name}.observer.csv"
        clocks = read_csv(clocks_path) if clocks_path.exists() else []
        row["post_draw_clock_samples"] = len(clocks)
        row["post_draw_engine_max_fps_values"] = sorted(set(column(clocks, "engine_max_fps")))
        row["post_draw_vsync_mode_values"] = sorted(set(column(clocks, "vsync_mode")))
        if (len(clocks) < row.get("frames_window", 1) or not clocks
                or row["post_draw_engine_max_fps_values"] != [0.0] or row["post_draw_vsync_mode_values"] != [0.0]):
            row["measurement_error"] = "post-draw CSV does not demonstrate uncapped max_fps=0 and vsync=0 for the completed measurement"
    if definition and definition.get("phase") == "measure" and definition.get("observer") != "buffered-post-draw-v2":
        row["measurement_error"] = "superseded observer protocol; periodic observer I/O was not qualified"
    row["log_mode"] = definition.get("log_mode", "file")
    if definition.get("log_mode") == "file" and not log_path.exists():
        row["measurement_error"] = "file logger control has no retained output"
    if row["log_mode"] == "nul":
        row["log_bytes"] = None
        row["error_markers_total"] = None
        row["shader_instance_limit_errors"] = None
        if log_path.exists():
            row["measurement_error"] = "NUL control unexpectedly produced a redirected log file"
    if row["log_mode"] == "pipe":
        receipt = directory / f"{name}.collector.json"
        collector = json.loads(receipt.read_text()) if receipt.exists() else {}
        row["collector"] = collector
        if (not log_path.exists() or collector.get("bytes") != row.get("log_bytes")
                or collector.get("sha256") != hashlib.sha256(log_path.read_bytes()).hexdigest()
                or collector.get("exit_code") != 0 or collector.get("suppressed_bytes") != 0):
            row["measurement_error"] = "pipe collector did not prove complete output and a successful viewer exit"
    row.update(process_stats(directory, name, first))
    if row["log_mode"] == "nul":
        row.pop("log_growth_post_startup", None)
    elif csv_path.exists() and window:
        row.update(window_log_stats(directory, name, window, native_begin_frame=scene["kind"] == "live"))
    proc_path = directory / f"{name}.proc.csv"
    info_path = directory / f"{name}.proc.info"
    if row.get("log_bytes") is not None and proc_path.exists() and info_path.exists():
        span = proc_path.stat().st_mtime - info_path.stat().st_mtime
        if span > 0:
            row["log_rate_full_sampled_process"] = dict(bytes_per_s=row["log_bytes"] / span,
                seconds=span, method="retained total bytes / sampler file-write span, including startup and teardown; approximate process lifetime")
    if row.get("log_bytes") and proc_path.exists() and max(column(read_csv(proc_path), "log_bytes"), default=0) == 0:
        row.pop("log_growth_post_startup", None)
        row.pop("log_growth_window", None)
        row["log_rate_error"] = "live size counter stayed zero despite retained output; rate is unavailable"
    elif row["log_mode"] == "pipe" and definition.get("log_counter") not in ("stream-eof-v2", "stream-markers-v3"):
        row.pop("log_growth_post_startup", None)
        row.pop("log_growth_window", None)
        row["log_rate_error"] = "legacy directory metadata can lag live pipe writes; selected-window rate is unavailable"
    report = directory / f"{name}.json"
    if report.exists():
        data = json.loads(report.read_text(encoding="utf-8-sig"))
        expected_start = (definition.get("engagement") or {}).get("start_identity")
        if expected_start and start_identity(data) != expected_start:
            row["measurement_error"] = "native measurement start differs from the prepared engagement fixture"
        if expected_start:
            row.update(order_rejections(data.get("live_session", {}), definition.get("engagement") or {}))
            row["order_rejection_classification"] = "tick/player matches are possible fixture rejections; other commands are AI in this read-only harness; unparseable diagnostics remain unattributed"
            if row["fixture_order_rejections"]:
                row["measurement_error"] = "native measurement rejected commands at a scripted fixture tick/player"
            elif row["unattributed_order_rejections"]:
                row["measurement_error"] = "native measurement has unattributed order rejections; fixture acceptance cannot be verified"
        if scene.get("mod_kind") == "raw":
            live = data.get("live_session", {})
            fleet = Counter((int(r["player"]), r["type"].casefold()) for r in live.get("start_fleet", []))
            free = Counter((int(r["player"]), r["type"].casefold()) for r in live.get("free_starting_forces", []))
            added = fleet - free
            expected = Counter((player, kind.casefold()) for player, names in scene["fleets"].items() for kind in names)
            row["added_fleet_matches_definition"] = added == expected
            row["actual_added_fleet"] = [dict(player=player, type=kind, count=count)
                                         for (player, kind), count in sorted(added.items())]
            if added != expected:
                row["measurement_error"] = "native start_fleet minus free_starting_forces does not match the RaW fleet definition"
        placements = data.get("populate", {}).get("placements", [])
        # Counts are authored/composed slots in the final population report, not
        # the live intact hardpoints or a per-frame hardware draw attribution.
        by_type = {}
        for placement in placements:
            kind = placement.get("object", "?")
            entry = by_type.setdefault(kind, dict(placements=0, hardpoint_slots=0))
            entry["placements"] += 1
            entry["hardpoint_slots"] += placement.get("hardpoints", 0)
        row["population_by_type"] = by_type
        row["hardpoint_slots_composed"] = sum(p.get("hardpoints", 0) for p in placements) if placements else None
    elif scene.get("mod_kind") == "raw":
        row["measurement_error"] = "missing native report; the actual RaW starting fleet cannot be verified"
    return row


def render_markdown(rows: list[dict], meta: dict) -> str:
    def f(value, digits=2):
        if isinstance(value, dict):
            value = value.get("median")
        return "-" if value is None or (isinstance(value, float) and math.isnan(value)) else f"{value:.{digits}f}"

    lines = [f"# Remake bench {meta.get('id', '')}", "",
             f"Host {meta.get('machine', '?')}, viewer {meta.get('viewer', '?')}, pacing {meta.get('pacing', '?')}; "
             "Frames use each scene's tick/frame window; private memory uses post-startup process samples.", "",
             "| Scene | median frame ms | p95 frame ms | viewer main scope ms | native tick ms | render CPU ms | GPU ms | draws | units | "
             "private GiB | first frame s |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in rows:
        frame = row.get("frame_ms", {})
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            row["scene"], f(frame.get("median") if frame else row.get("result_median_frame_ms")),
            f(frame.get("p95") if frame else None), f(row.get("main_ms")), f(row.get("tick_ms")), f(row.get("render_cpu_ms")),
            f(row.get("gpu_ms")), f(row.get("draws"), 0), f(row.get("units"), 0),
            f(row.get("private_gib_median")), f(row.get("first_frame_s"), 1)))
    errors = [f"{r['scene']}: {r['measurement_error']}" for r in rows if r.get("measurement_error")]
    if errors:
        lines += ["", "INVALID measurements: " + "; ".join(errors)]
    return "\n".join(lines) + "\n"


def per_ship_table(rows: list[dict]) -> str:
    """Report additive scene totals; a causal per-ship cost needs a controlled spawn hook."""
    by = {r["scene"]: r for r in rows}
    def metric(value):
        if isinstance(value, dict):
            value = value.get("median")
        return "-" if value is None else f"{value:.2f}"
    lines = ["", "## Controlled-scene totals (fleet setup; console-spawn parity unverified)", "",
             "| Scene | viewer main scope ms | draws | units | composed hardpoint slots | active effects | effect particles | p95 frame ms |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for name in [n for n in by if n.startswith("solo-") or n.startswith("remake-dev")]:
        r = by[name]
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} |".format(
            name, *((metric(r.get(k)))
                    for k in ("main_ms", "draws", "units", "hardpoint_slots_composed", "effects", "effect_particles")),
            metric(r.get("frame_ms", {}).get("p95"))))
    dev = [(1, "remake-dev1"), (2, "remake-dev2"), (4, "remake-dev4")]
    pts = [(n, by[s]["main_ms"]["median"]) for n, s in dev if s in by and by[s].get("main_ms", {}).get("median") is not None]
    if len(pts) >= 2:
        (n0, m0), (n1, m1) = pts[0], pts[-1]
        lines += ["", f"Descriptive Devastator scene slope: {(m1 - m0) / (n1 - n0):.2f} ms per added ship "
                      f"(from {n0} to {n1} ships, median viewer_main_ms; battle states differ, so this is not isolated ship cost)."]
    return "\n".join(lines) + "\n"


def analyse(directory: pathlib.Path, meta: dict) -> bool:
    meta = dict(meta)
    for filename, label in (("rig.json", "rig"), ("viewer-artifact-source.json", "artifact_source")):
        path = directory / filename
        if path.exists():
            meta[label] = json.loads(path.read_text(encoding="utf-8-sig"))
    if "rig" in meta:
        meta["machine"] = meta["rig"]["machine"]
    if "artifact_source" in meta:
        source = meta["artifact_source"]
        manifest = pathlib.Path(source.get("manifest", ""))
        details = json.loads(manifest.read_text(encoding="utf-8-sig")) if manifest.is_file() else source
        meta["viewer"] = details.get("builtCommit", details.get("commit"))
        meta["viewer_dll_sha256"] = details.get("sha256")
    spec = directory / "_spec.json"
    requested = [r["name"] for r in json.loads(spec.read_text(encoding="utf-8-sig"))["runs"]] if spec.exists() else []
    rows = []
    for name, scene in SCENES.items():
        if name in requested or any((directory / f"{name}{suffix}").exists() for suffix in (".json", ".csv", ".observer.csv")):
            rows.append(analyse_scene(directory, name, scene))
    if not rows:
        raise SystemExit(f"no scene output in {directory}")
    protocol_errors = []
    if "rig" in meta and not meta["rig"].get("privateDesktop"):
        protocol_errors.append("the rig did not use a private desktop")
    if meta.get("rig", {}).get("timedOut", False):
        protocol_errors.append("the rig timed out")
    valid = not any(r.get("measurement_error") for r in rows) and not protocol_errors
    (directory / "results.json").write_text(json.dumps(dict(meta=meta, valid=valid, protocol_errors=protocol_errors, scenes=rows), indent=2, allow_nan=False), encoding="utf-8")
    text = render_markdown(rows, meta) + per_ship_table(rows)
    if protocol_errors:
        text += "\nINVALID run protocol: " + "; ".join(protocol_errors) + "\n"
    (directory / "results.md").write_text(text, encoding="utf-8")
    print(text)
    return valid


def invoke_rig(args: argparse.Namespace, spec: pathlib.Path, output: pathlib.Path) -> tuple[int, pathlib.Path | None]:
    command = ["pwsh", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-File",
               str(pathlib.Path(__file__).resolve().parents[2] / "tools/rig/Invoke-RigViewer.ps1"),
               "-Spec", str(spec), "-PrivateDesktop", "-OutRoot", str(output),
               "-TimeoutSeconds", str(args.timeout), "-QueueTimeoutSeconds", str(args.queue_timeout)]
    if args.viewer_artifact:
        command += ["-ViewerArtifact", args.viewer_artifact]
    command += ["-Machine", ",".join(args.machine)]
    code = subprocess.run(command, check=False, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).returncode
    runs = sorted(p for p in output.glob("*") if p.is_dir() and (p / "rig.json").exists())
    return code, runs[-1] if runs else None


def load_preparations(directory: pathlib.Path, names: list[str]) -> dict:
    reports = {}
    for name in names:
        path = directory / f"{name}.json"
        if not path.is_file() and SCENES[name].get("prepare_source"):
            path = directory / f"{SCENES[name]['prepare_source']}.json"
        if not path.is_file():
            raise ValueError(f"missing native preparation report: {path}")
        reports[name] = json.loads(path.read_text(encoding="utf-8-sig"))
        engagement_recipe(reports[name])
    return reports


def remote_rig_hosts() -> set[str]:
    """Names and aliases of the remote GPU hosts in the host table (never the local workstation)."""
    root = pathlib.Path(__file__).resolve().parents[2]
    config = pathlib.Path(os.environ.get("EAWR_GPU_HOSTS") or root / "config/gpu-hosts.local.json")
    if not config.is_file():
        config = root / "config/gpu-hosts.json"
    hosts = json.loads(config.read_text(encoding="utf-8-sig"))["hosts"]
    return {name.casefold() for host in hosts if host.get("connection") != "local"
            for name in [host["name"], *host.get("aliases", [])]}


def run(args: argparse.Namespace) -> int:
    allowed = remote_rig_hosts()
    if not args.machine or any(machine.casefold() not in allowed for machine in args.machine):
        raise SystemExit("Choose explicit modded remote rig hosts from the host table: " + ", ".join(sorted(allowed)))
    if not args.viewer_artifact and not args.plan_only:
        raise SystemExit("A retained --viewer-artifact is required; the benchmark does not request a new build")
    scenes = select(args.scenes)
    live = [name for name in scenes if SCENES[name]["kind"] == "live"]
    out = pathlib.Path(args.out or f"out/remake-bench/{time.strftime('%Y%m%d-%H%M%S')}").resolve()
    spec = out / "spec.json"
    preparations = None
    if live:
        if args.prepare_from:
            preparations = load_preparations(pathlib.Path(args.prepare_from), live)
        else:
            prepare_spec = out / "prepare-spec.json"
            write_spec(prepare_spec, live, "free", args.mod_root, args.raw_mod_root, prepare=True)
            print(f"native preparation spec {prepare_spec}; meeting-point orders resolve from its reports", flush=True)
            if args.plan_only:
                return 0
            code, directory = invoke_rig(args, prepare_spec, out / "prepare")
            if code or directory is None:
                return code or 1
            preparations = load_preparations(directory, live)
    write_spec(spec, scenes, args.pacing, args.mod_root, args.raw_mod_root, preparations=preparations)
    print(f"measurement spec {spec} ({len(scenes)} scenes: {', '.join(scenes)})", flush=True)
    if args.plan_only:
        return 0
    code, directory = invoke_rig(args, spec, out / "rig")
    if directory is not None:
        meta = dict(id=out.name, machine=args.machine, viewer=args.viewer_artifact, pacing=args.pacing,
                    exit_code=code, seed=SEED)
        valid = analyse(directory, meta)
        return code or (0 if valid else 1)
    return code or 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p_run = sub.add_parser("run", help="write the spec, run it on a GPU host, analyse")
    p_run.add_argument("--viewer-artifact", help="viewer-artifact.json from windows_build.py --viewer")
    p_run.add_argument("--mod-root", default=os.environ.get("EAWR_BENCH_MOD_ROOT"),
                       help="explicit leaf-first semicolon-separated Remake roots on the GPU host")
    p_run.add_argument("--raw-mod-root", default=os.environ.get("EAWR_BENCH_RAW_MOD_ROOT"),
                       help="explicit RaW roots on the GPU host (raw-late-fleet group)")
    p_run.add_argument("--machine", action="append", default=[], help="GPU host name (repeatable); same host for before/after")
    p_run.add_argument("--scenes", default="smoke", help="smoke | full | ship | raw-late-fleet | comma list of: " + ", ".join(SCENES))
    p_run.add_argument("--pacing", choices=("audio", "free"), default="free",
                       help="free = uncapped/vsync off; audio = legacy paced control")
    p_run.add_argument("--prepare-from", help="reuse native start reports from this directory; otherwise prepare them on the rig first")
    p_run.add_argument("--out")
    p_run.add_argument("--timeout", type=int, default=7200)
    p_run.add_argument("--queue-timeout", type=int, default=14400)
    p_run.add_argument("--plan-only", action="store_true")
    p_an = sub.add_parser("analyse", help="analyse a finished rig run directory")
    p_an.add_argument("directory", type=pathlib.Path)
    sub.add_parser("list", help="list the scenes")
    parsed = parser.parse_args()
    if parsed.command == "list":
        for name, scene in SCENES.items():
            print(f"{name:28} {','.join(scene['tags']):14} {scene['note']}")
        return 0
    if parsed.command == "analyse":
        return 0 if analyse(parsed.directory, dict(id=parsed.directory.name)) else 1
    return run(parsed)


if __name__ == "__main__":
    sys.exit(main())
