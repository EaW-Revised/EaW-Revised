"""Prepare and run a repeatable all-AI galaxy capture through the unpinned rig queue.

All derived XML, reports and media belong in ignored out/. This is capture tooling;
installed and tracked game data are never modified. Use --frames 5400 for phase 1.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def overlay_from_sources(sources, campaign, output, ai_controller=None):
    """Find the requested definition in the effective corpus; preserve authored references."""
    matches, campaigns = [], []
    for source in sources:
        try:
            tree = ET.fromstring(source.data)
        except ET.ParseError:
            continue
        for row in tree:
            if row.tag.lower() != "campaign":
                continue
            campaigns.append(row)
            if row.attrib.get("Name", "").casefold() == campaign.casefold():
                matches.append((source.logical_path, tree, row))
    if len(matches) != 1:
        raise ValueError(f"expected one effective campaign definition, found {len(matches)}: {campaign}")
    logical, tree, row = matches[0]
    # A human campaign variant omits its active faction's AI mapping. Reuse the
    # same faction's authored controller from a variant of this exact galaxy.
    # Preserve all other controls, including authored passive/None factions.
    active = (row.findtext("Starting_Active_Player") or "").strip()
    def references(item, tag):
        return tuple(sorted(value.strip().casefold() for value in (item.findtext(tag) or "").split(",") if value.strip()))
    def controls(item):
        return [(control, [value.strip() for value in (control.text or "").split(",")])
                for control in item.findall("AI_Player_Control")]
    local = next((control for control, values in controls(row) if len(values) == 2 and values[0].casefold() == active.casefold()), None)
    current = (local.text or "").split(",")[-1].strip() if local is not None else ""
    if active and (ai_controller or not current or current.casefold() == "none"):
        candidates = {values[1] for variant in campaigns
                      if references(variant, "Locations") == references(row, "Locations")
                      and references(variant, "Trade_Routes") == references(row, "Trade_Routes")
                      for _, values in controls(variant) if len(values) == 2
                      and values[0].casefold() == active.casefold() and values[1].casefold() != "none"}
        if not ai_controller:
            if len(candidates) != 1:
                raise ValueError(f"active faction {active} needs --ai-controller; authored same-galaxy candidates: {sorted(candidates)}")
            ai_controller = next(iter(candidates))
        if local is None:
            local = ET.SubElement(row, "AI_Player_Control")
        local.text = f"{active}, {ai_controller}"
    # Campaign perception stays authored; the capture viewer reveals its own view.
    destination = Path(output).resolve()
    target = destination / logical
    if not target.resolve().is_relative_to(destination):
        raise ValueError("campaign XML path escapes overlay")
    target.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(tree).write(target, encoding="utf-8", xml_declaration=True)
    (destination / "Data").mkdir(exist_ok=True)
    (destination / "Data" / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    return destination


def make_overlay(game_root, campaign, output, ai_controller=None):
    from tools.inventory.corpus import Corpus
    return overlay_from_sources(Corpus(game_root).iter_sources("foc", ".xml", "effective"), campaign, output, ai_controller)


def make_spec(campaign, overlay, frames=5400, seed=1, video=False):
    if frames <= 0:
        raise ValueError("frames must be positive")
    runs = []
    for name in ("repeat-a", "repeat-b"):
        run = {"name": name, "capture": True, "args": [
            "--eawr-galactic", campaign, "--eawr-mod-root", "{in}/campaign-overlay",
            "--eawr-galactic-paused", "no", "--eawr-galactic-all-ai", "yes",
            "--eawr-galactic-seed", str(seed), "--eawr-galactic-ticks", str(frames)]}
        if video:
            run["engineArgs"] = ["--write-movie", "{out}/" + name + ".avi", "--fixed-fps", "30"]
        runs.append(run)
    return {"inputs": {"campaign-overlay": str(overlay)}, "runs": runs}


def validate_report(data, frames):
    if data.get("status") != "captured":
        raise ValueError(f"capture failed: {data.get('failure')}")
    proof = data["ai_proof"]
    if not proof["all_ai"] or not proof["event_feed_complete"] or not proof["end_state_hash"]:
        raise ValueError("all-AI or complete end-state evidence missing")
    expected = [0, *range(1800, frames + 1, 1800)]
    if expected[-1] != frames:
        expected.append(frames)
    if [row["galactic_frame"] for row in proof["samples"]] != expected:
        raise ValueError("per-minute report has missing or unexpected game-time samples")
    if data["proof_capture_frames"] != expected[1:]:
        raise ValueError("timed galaxy-map capture series is incomplete")
    return proof


def capture_timeout(spec):
    """Budget the whole rig job at ten wall seconds per game second plus startup slack."""
    frames = []
    for run in spec["runs"]:
        args = run["args"]
        frames.append(int(args[args.index("--eawr-galactic-ticks") + 1]))
    if not frames or min(frames) <= 0:
        raise ValueError("capture runs must have positive frame counts")
    return max(3600, sum((count + 2) // 3 + 600 for count in frames))


def collect(directory, output, frames):
    directory, output = Path(directory), Path(output)
    reports = [json.loads((directory / f"repeat-{name}.json").read_text(encoding="utf-8-sig")) for name in ("a", "b")]
    proofs = [validate_report(data, frames) for data in reports]
    if proofs[0] != proofs[1]:
        raise ValueError("same-host same-seed repeats differ in state or cumulative metric report")
    for name, data in zip(("a", "b"), reports):
        for frame in data["proof_capture_frames"]:
            png = directory / f"repeat-{name}-frame-{frame}.png"
            if not png.is_file() or png.stat().st_size < 100:
                raise ValueError(f"missing galaxy-map capture: {png}")
    names = {row["player"]: row["faction"] for row in reports[0]["ai_proof_factions"]}
    report = dict(proofs[0], campaign=reports[0]["campaign"], seed=reports[0]["ai_proof_seed"],
                  deterministic_repeats=True, capture_directory=str(directory), frames=frames)
    for sample in report["samples"]:
        for player in sample["players"]:
            player["faction"] = names.get(player["player"], str(player["player"]))
    output.mkdir(parents=True, exist_ok=True)
    (output / "per-minute.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = ["# Live all-AI campaign smoke", "", f"Campaign: {report['campaign']}; seed {report['seed']}; "
             f"{frames / 30:g} in-game seconds. Same-host repeats: equal end-state hash and metrics.", "",
             f"End-state hash: `{report['end_state_hash']}`.", "",
             "| Faction | Credits | Planets | Built space/ground | Fleet departures | Attacks | Battles won/lost | Tech | Maximum idle seconds |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for p in report["samples"][-1]["players"]:
        name = p["faction"].replace("|", "\\|").replace("\n", " ")
        built = f"{p['units_built_space']}/{p['units_built_ground']}" if p["units_built_available"] else "unavailable"
        lines.append(f"| {name} | {p['credits_raw'] / (1 << 24):g} | {p['planets_owned']} | "
                     f"{built} | {p['fleet_moves']} | {p['attacks']} | "
                     f"{p['battles_won']}/{p['battles_lost']} | {p['tech']} | {p['maximum_idle_frames'] / 30:g} |")
    lines += ["", "Per-minute cumulative metrics: per-minute.json. Plans and simulation-only frame time have no public live observer yet; "
              "they are marked unavailable. This short tooling smoke does not evaluate the 30-minute AI acceptance gate."]
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("--game-root", required=True)
    prepare.add_argument("--campaign", required=True)
    prepare.add_argument("--out", type=Path, required=True)
    prepare.add_argument("--ai-controller", help="explicit authored controller if same-galaxy variants are ambiguous")
    prepare.add_argument("--frames", type=int, default=5400)
    prepare.add_argument("--seed", type=int, default=1)
    prepare.add_argument("--video", action="store_true")
    run = sub.add_parser("run")
    run.add_argument("--spec", type=Path, required=True)
    run.add_argument("--viewer-artifact", type=Path, required=True)
    run.add_argument("--out", type=Path, required=True)
    harvest = sub.add_parser("collect")
    harvest.add_argument("--runs-dir", type=Path, required=True)
    harvest.add_argument("--out", type=Path, required=True)
    harvest.add_argument("--frames", type=int, default=5400)
    args = parser.parse_args(argv)
    if args.command == "prepare":
        output = args.out.resolve()
        if not output.is_relative_to(ROOT / "out"):
            parser.error("derived installed-data overlays must stay in this worktree's ignored out/")
        overlay = make_overlay(args.game_root, args.campaign, output / "overlay", args.ai_controller)
        spec = make_spec(args.campaign, overlay, args.frames, args.seed, args.video)
        (output / "capture-spec.json").write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
        print(output / "capture-spec.json")
    elif args.command == "run":
        # Both repeats occupy one unpinned rig job, guaranteeing one host.
        spec = json.loads(args.spec.read_text(encoding="utf-8-sig"))
        return subprocess.call(["pwsh", "-NoProfile", "-File", str(ROOT / "tools/rig/Invoke-RigViewer.ps1"),
            "-Spec", str(args.spec.resolve()), "-ViewerArtifact", str(args.viewer_artifact.resolve()),
            "-OutRoot", str(args.out.resolve()), "-TimeoutSeconds", str(capture_timeout(spec)), "-QueueTimeoutSeconds", "14400"], cwd=ROOT)
    else:
        report = collect(args.runs_dir, args.out, args.frames)
        print(report["end_state_hash"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
