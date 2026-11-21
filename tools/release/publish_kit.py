#!/usr/bin/env python3
"""Publish a finished owner test kit as a release of the PRIVATE game repository (the launcher's Private channel).

  python tools/release/publish_kit.py --kit <kit>.zip [--notes changelog.md] [--dry-run]

One command that:
  1. checks the kit: the zip's SHA-256 against its `.zip.sha256` and its `.zip.manifest.json` sidecar (the one
     `finish_kit.py` delivers; a release-mode sidecar is refused);
  2. refuses unless the target repository is private;
  3. derives the tag `kit-<release version>-kit.<n>` (n = next free number for that version among the repository's
     existing kit-* releases and tags);
  4. writes `update-manifest.json` for channel `private` with the same generator the public release uses
     (`update_manifest.py`) and a `SHA256SUMS` of the zip and the manifest;
  5. creates the GitHub release with `gh`: NOT a draft, NOT a pre-release, `--latest=false`, target = the kit commit
     (the plan's base commit when the kit commit exists only locally), assets = zip + manifest + SHA256SUMS;
  6. prints the release URL.

It never creates a release or a tag when a check fails. `--dry-run` runs every read-only check, writes the manifest and
sums into the work folder, prints the exact `gh` command and creates nothing. The repository comes from `--repo`, the
EAWR_KIT_REPO environment variable or this checkout's `origin` remote; no repository name is stored in the source.

Exit codes: 0 done (or dry run), 2 usage, 3 a check refused, 4 a gh call failed, 5 the release was created but
the final check disagrees (look at it by hand).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
import hidden_console  # noqa: E402

hidden_console.install()
import kit_manifest  # noqa: E402
import release_version  # noqa: E402
import update_manifest  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
REPO_ENV = "EAWR_KIT_REPO"
SUMS_NAME = "SHA256SUMS"
_REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_HEX40 = re.compile(r"[0-9a-f]{40}")
_NUMBER = re.compile(r"[1-9][0-9]{0,2}")


class Refused(Exception):
    """A check failed: nothing was created (exit 3)."""


class GhFailed(Exception):
    """A gh call failed (exit 4)."""


def gh(*args: str) -> str:
    """Run `gh <args>` and return its stdout. The one place that talks to GitHub: tests replace it."""
    result = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8", check=False)
    if result.returncode:
        raise GhFailed(f"gh {' '.join(args[:3])} failed ({result.returncode}): {(result.stderr or result.stdout).strip()[:400]}")
    return result.stdout


def gh_exists(*args: str) -> bool:
    """True when the gh api call succeeds, False on a 404; any other failure is raised."""
    try:
        gh(*args)
    except GhFailed as error:
        if any(marker in str(error) for marker in ("404", "Not Found", "No commit found", "422")):
            return False
        raise
    return True


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def json_pages(text: str) -> list:
    """The items of a `gh api --paginate` answer: one JSON array per page, concatenated."""
    decoder, items, index = json.JSONDecoder(), [], 0
    text = text.strip()
    while index < len(text):
        value, index = decoder.raw_decode(text, index)
        items += value if isinstance(value, list) else [value]
        while index < len(text) and text[index].isspace():
            index += 1
    return items


def repo_from_origin(checkout: Path) -> str:
    result = subprocess.run(["git", "-C", str(checkout), "remote", "get-url", "origin"], capture_output=True, text=True, check=False)
    match = re.search(r"github\.com[:/]([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?/?$", result.stdout.strip())
    if result.returncode or match is None:
        raise Refused("no repository: pass --repo owner/name (or set EAWR_KIT_REPO); this checkout's origin is not a GitHub address")
    return match.group(1)


def find_zip(kit: Path) -> Path:
    """The kit zip: the file itself, or the `<folder>.zip` delivered beside a kit folder."""
    if kit.is_dir():
        kit = kit.parent / (kit.name + ".zip")
    if not kit.is_file() or kit.suffix != ".zip":
        raise Refused(f"{kit}: pass the delivered kit .zip (or the kit folder beside its .zip)")
    return kit


def check_kit(zip_path: Path) -> tuple[dict, str]:
    """(sidecar manifest, zip SHA-256). Refuses a zip whose hash is not the one the finish step recorded."""
    digest = sha256_file(zip_path)
    hash_file = zip_path.with_name(zip_path.name + ".sha256")
    if not hash_file.is_file():
        raise Refused(f"{hash_file.name} is missing: the zip's hash cannot be checked")
    recorded = {}
    for line in hash_file.read_text(encoding="utf-8").splitlines():
        value, _, name = line.partition("  ")
        recorded[name.strip()] = value.strip()
    if recorded.get(zip_path.name) != digest:
        raise Refused(f"{zip_path.name}: SHA-256 {digest} does not match {hash_file.name} ({recorded.get(zip_path.name)})")
    sidecar = zip_path.with_name(zip_path.name[:-len(".zip")] + kit_manifest.SIDECAR_SUFFIX)
    if not sidecar.is_file():
        raise Refused(f"{sidecar.name} is missing: it names the kit commit and the release version")
    try:
        manifest = kit_manifest.read(sidecar)
    except (kit_manifest.ManifestError, ValueError) as error:
        raise Refused(f"{sidecar.name}: {error}") from error
    if manifest.get("mode") != "kit":
        raise Refused(f"{sidecar.name} is a {manifest.get('mode')!r} manifest: only owner test kits are published here")
    problems = kit_manifest.verify(manifest, zip_path=zip_path, require_finish=True)
    if problems:
        raise Refused(f"{sidecar.name} does not describe this zip: " + "; ".join(problems[:4]))
    return manifest, digest


def kit_numbers(version: str, tags: set[str]) -> list[int]:
    pattern = re.compile(re.escape(f"{update_manifest.KIT_PREFIX}{version}-kit.") + r"([0-9]+)")
    return [int(m.group(1)) for tag in tags if (m := pattern.fullmatch(tag))]


def existing_tags(repo: str) -> set[str]:
    """Every tag a kit release or a bare kit-* tag already uses (drafts included)."""
    tags = {str(r.get("tag_name", "")) for r in json_pages(gh("api", "--paginate", f"repos/{repo}/releases?per_page=100"))}
    refs = json_pages(gh("api", "--paginate", f"repos/{repo}/git/matching-refs/tags/{update_manifest.KIT_PREFIX}"))
    tags |= {str(r.get("ref", "")).removeprefix("refs/tags/") for r in refs}
    return tags


def pick_target(repo: str, kit_sha: str, base_sha: str) -> tuple[str, str]:
    """(commit, note). The kit commit when the repository has it; its base commit when only that is pushed."""
    if gh_exists("api", f"repos/{repo}/commits/{kit_sha}", "--jq", ".sha"):
        return kit_sha, ""
    if gh_exists("api", f"repos/{repo}/commits/{base_sha}", "--jq", ".sha"):
        return base_sha, (f"note: the kit commit {kit_sha[:12]} is not in {repo} (kit commits are local); "
                          f"the release targets its base commit {base_sha[:12]}")
    raise Refused(f"neither the kit commit {kit_sha[:12]} nor its base {base_sha[:12]} exists in {repo}: push the base, or pass --target")


def plan(args: argparse.Namespace) -> dict:
    """Run every check and prepare every file; nothing is written to GitHub."""
    zip_path = find_zip(args.kit)
    manifest, zip_hash = check_kit(zip_path)
    source = manifest["plan"]["source"]
    kit_sha = source["kit_sha"]

    repo = args.repo or os.environ.get(REPO_ENV) or repo_from_origin(ROOT)
    if _REPO.fullmatch(repo) is None:
        raise Refused(f"{repo!r} is not owner/name")
    info = json.loads(gh("repo", "view", repo, "--json", "isPrivate,visibility,nameWithOwner"))
    if info.get("isPrivate") is not True:
        raise Refused(f"{repo} is {str(info.get('visibility', 'not private')).lower()}: kits are only published to a private repository")

    base_version = args.release_version or manifest["plan"]["version"].get("release_version")
    if not base_version or release_version.VERSION.fullmatch(base_version) is None:
        raise Refused("the kit's release version is unknown (an older kit commit): pass --release-version <major.minor.patch>")
    tags = existing_tags(repo)
    number = args.number or max(kit_numbers(base_version, tags), default=0) + 1
    version = f"{base_version}-kit.{number}"
    tag = update_manifest.KIT_PREFIX + version
    if tag in tags or gh_exists("api", f"repos/{repo}/git/ref/tags/{tag}"):
        raise Refused(f"the tag {tag} already exists in {repo}: nothing was created")

    target, target_note = (args.target, "") if args.target else pick_target(repo, kit_sha, source["base_sha"])

    notes = args.notes.read_text(encoding="utf-8") if args.notes else ""
    body = notes.strip() or f"Private test kit {version}, built from commit {kit_sha[:12]}."
    built = update_manifest.build(zip_path, version, kit_sha, channel=update_manifest.KIT_CHANNEL, notes=body)
    update_manifest.validate(built, expected_tag=tag, expected_channel=update_manifest.KIT_CHANNEL)
    return {"repo": repo, "zip": zip_path, "zip_sha256": zip_hash, "tag": tag, "version": version, "target": target,
            "target_note": target_note, "kit_sha": kit_sha, "body": body, "manifest": built}


def stage(prepared: dict, work: Path) -> dict[str, Path]:
    """Write the manifest, the sums and the release notes into `work`; returns the files by role."""
    work.mkdir(parents=True, exist_ok=True)
    manifest_path = work / update_manifest.MANIFEST_NAME
    manifest_path.write_text(update_manifest.render(prepared["manifest"]), encoding="utf-8", newline="\n")
    sums_path = work / SUMS_NAME
    zip_path = prepared["zip"]
    sums_path.write_text(f"{prepared['zip_sha256']}  {zip_path.name}\n{sha256_file(manifest_path)}  {manifest_path.name}\n",
                         encoding="utf-8", newline="\n")
    notes_path = work / "release-notes.md"
    notes_path.write_text(prepared["body"] + "\n", encoding="utf-8", newline="\n")
    return {"manifest": manifest_path, "sums": sums_path, "notes": notes_path}


def create_command(prepared: dict, files: dict[str, Path]) -> list[str]:
    """The exact gh call. No --prerelease and no --draft: the launcher lists non-draft kit-* tags only, and
    --latest=false keeps a kit from ever becoming the repository's latest release."""
    return ["release", "create", prepared["tag"], str(prepared["zip"]), str(files["manifest"]), str(files["sums"]),
            "--repo", prepared["repo"], "--target", prepared["target"], "--title", f"Private test kit {prepared['version']}",
            "--notes-file", str(files["notes"]), "--latest=false"]


def final_check(prepared: dict) -> list[str]:
    """What the created release looks like to the launcher: published, not a pre-release, not latest, three assets."""
    repo, tag = prepared["repo"], prepared["tag"]
    release = json.loads(gh("api", f"repos/{repo}/releases/tags/{tag}"))
    problems = []
    if release.get("draft"):
        problems.append("it is a draft (the launcher ignores drafts)")
    if release.get("prerelease"):
        problems.append("it is marked pre-release")
    wanted = {prepared["zip"].name, update_manifest.MANIFEST_NAME, SUMS_NAME}
    missing = wanted - {a.get("name") for a in release.get("assets", [])}
    if missing:
        problems.append(f"assets missing: {sorted(missing)}")
    try:
        latest = json.loads(gh("api", f"repos/{repo}/releases/latest")).get("tag_name")
    except GhFailed:
        latest = None  # no published public-style release: nothing is latest
    if latest == tag:
        problems.append("it is the repository's latest release")
    return problems


def run(args: argparse.Namespace) -> int:
    prepared = plan(args)
    # A dry run keeps what it staged so the operator can read the exact files; a real run stages in a folder it removes.
    scratch = None if args.work or args.dry_run else tempfile.TemporaryDirectory(prefix="eawr-kit-publish-")
    work = args.work or (Path(scratch.name) if scratch else Path(tempfile.mkdtemp(prefix="eawr-kit-publish-")))
    try:
        files = stage(prepared, work)
        command = create_command(prepared, files)
        manifest = prepared["manifest"]
        print(f"kit      {prepared['zip']}")
        print(f"zip      {prepared['zip_sha256']}  {prepared['zip'].stat().st_size} bytes")
        print(f"repo     {prepared['repo']} (private)")
        print(f"tag      {prepared['tag']}   (version {prepared['version']}, channel {manifest['channel']}, build id {manifest['build_id']})")
        print(f"target   {prepared['target']}")
        if prepared["target_note"]:
            print(prepared["target_note"])
        print(f"files    {len(manifest['files'])} listed in {update_manifest.MANIFEST_NAME}; assets: "
              f"{prepared['zip'].name}, {files['manifest'].name}, {files['sums'].name}")
        print(f"staged   {work}")
        print("command  gh " + " ".join(f'"{a}"' if " " in a else a for a in command))
        if args.dry_run:
            print("DRY RUN: nothing was created (no release, no tag).")
            return 0
        try:
            gh(*command)
        except GhFailed as error:  # never retried: the tag may exist now
            raise GhFailed(f"{error} (the release may be partly created: look at {prepared['repo']} for "
                           f"{prepared['tag']} before running this again)") from error
        url = f"https://github.com/{prepared['repo']}/releases/tag/{prepared['tag']}"
        try:
            url = gh("release", "view", prepared["tag"], "--repo", prepared["repo"], "--json", "url", "--jq", ".url").strip() or url
            problems = final_check(prepared)
        except (GhFailed, ValueError) as error:
            problems = [f"the final check could not run: {error}"]
        print(f"release  {url}")
        if problems:
            print("WARNING: the release exists but: " + "; ".join(problems), file=sys.stderr)
            return 5
        return 0
    finally:
        if scratch is not None:
            scratch.cleanup()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     epilog="Exit codes: 0 done, 2 usage, 3 a check refused, 4 a gh call failed, 5 created but the final check disagrees.")
    parser.add_argument("--kit", required=True, type=Path, help="the delivered kit .zip (or the kit folder beside its .zip)")
    parser.add_argument("--notes", type=Path, help="changelog (markdown) for the release body and the manifest notes the launcher shows")
    parser.add_argument("--repo", help=f"owner/name of the private repository (default: ${REPO_ENV}, else this checkout's origin)")
    parser.add_argument("--release-version", help="override the kit's release version (an older kit commit without one)")
    parser.add_argument("--number", type=int, help="force the kit number n instead of the next free one (the tag must still be free)")
    parser.add_argument("--target", help="commit the release points at (default: the kit commit, else its base commit)")
    parser.add_argument("--work", type=Path, help="folder for the staged manifest, sums and notes (kept; default: a temporary one)")
    parser.add_argument("--dry-run", action="store_true", help="run every read-only check and print the exact gh command; create nothing")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.number is not None and _NUMBER.fullmatch(str(args.number)) is None:
        print("publish_kit: --number must be 1..999", file=sys.stderr)
        return 2
    if args.target is not None and _HEX40.fullmatch(args.target) is None:
        print("publish_kit: --target must be a full lowercase commit hash", file=sys.stderr)
        return 2
    if args.notes is not None and not args.notes.is_file():
        print(f"publish_kit: {args.notes} does not exist", file=sys.stderr)
        return 2
    try:
        return run(args)
    except Refused as error:
        print(f"publish_kit: refused: {error}", file=sys.stderr)
        return 3
    except GhFailed as error:
        print(f"publish_kit: {error}", file=sys.stderr)
        return 4
    except (ValueError, OSError) as error:  # update_manifest.build/validate and file errors
        print(f"publish_kit: refused: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
