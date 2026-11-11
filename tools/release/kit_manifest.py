#!/usr/bin/env python3
"""The kit manifest: one JSON file that says what a kit was cut from, by whom, and what came out.

A kit is cut from a *kit commit* (the base commit plus a label, the launcher pin and any extra files). The manifest
pins everything that decides the kit's bytes, in three sections, each written by a different step and each carrying
the hash of the one before it:

  plan    written by cut_kit.py before any job is queued: the commit, the files the packaging and finish steps run
          or read (git blob ids and SHA-256), the engine, launcher and Revised-layer identities, the orchestrator
          that moved the bytes (its commit, whether it was clean, the streamed scripts' hashes) and the route.
  host    written by cut_kit.py from what the build host returned: the player archive's name, size and SHA-256.
  finish  written by finish_kit.py: the checks that passed and the delivered archive and SHA256SUMS hashes.

`verify` re-derives every hash it can without trusting the file: it recomputes the plan hash, compares the section
links, the archive and folder bytes, and (with --repo) the git objects the plan names. It uses only the standard
library, so anyone can run it on a delivered kit:

  python kit_manifest.py verify <manifest> [--zip <kit.zip>] [--folder <kit folder>] [--repo <checkout>]
  python kit_manifest.py show <manifest>

The manifest is deliberately not inside the kit folder: the kit's file list stays the player layout.

What it does and does not prove. It is hash-pinned, not signed (signing is out of scope, like the rest of the unsigned
release policy). Each section's hash catches accidental or partial edits, and `verify --repo` checks the plan against git
objects (the kit commit, its tree, its parent, the files it changed, every payload blob), so a plan cannot name files the
commit does not have. Someone who can rewrite the manifest, the zip and the sums together can still produce a set that
verifies for a player they built themselves: pin the commit you expect with `--expect-base` and keep the delivered set
where only you can write. The `host` section is host-attested: the client checks the downloaded archive against what the
host's own log and bundle said, so it shows what arrived, not that the host built it honestly.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

SCHEMA = "eawr-kit-manifest/1"
MANIFEST_NAME = "kit-manifest.json"
SIDECAR_SUFFIX = ".zip.manifest.json"
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")

# Files that decide a kit's bytes or the checks on them. A required file missing from the kit commit refuses the cut.
PAYLOAD_REQUIRED = (
    "tools/release/package_release.py", "tools/release/kit_engine.py", "tools/release/launcher_artifact.py",
    "tools/release/update_manifest.py", "tools/release/add_revised_layer.py", "tools/release/kit-readme.template.md",
    "tools/release/revised-layer-manifest.json", "tools/engine_pins.json", "tools/common/engine_guard.py",
    "apps/viewer/tools/export_package.py", "config/launcher-pin.json",
    # finish runs these two from the operator's checkout and refuses to run unless they are byte-equal to the kit commit's.
    "tools/release/finish_kit.py", "tools/release/kit_manifest.py",
)
PAYLOAD_OPTIONAL = (
    "tools/release/package_cross.py", "tools/release/godot_wine.py",
    "tools/release/cut_kit.py", "tools/release/host_package.py",
    "tools/release/pool_host/Package-OnHost.ps1",
    "config/release-version.json",
)
# What finish_kit extracts from the kit commit to run its checks from.
PAYLOAD_TREES = ("tools", "apps/viewer/tools", "config", "LICENSE", "THIRD_PARTY_NOTICES.md",
                 "docs/player-guide.md", "docs/debug-build.md")
# The scripts and tools that move bytes for a cut. They run from the operator's checkout, so the plan records them.
ORCHESTRATOR_FILES = (
    "tools/release/cut_kit.py", "tools/offload/windows_build.py", "tools/offload/linux_cross_build.py",
    "tools/offload/offload_job.py", "tools/offload/offload_common.py", "tools/offload/cross_package.py", "tools/offload/package_plan.py",
    "tools/offload/windows_remote_build.ps1", "tools/offload/linux_cross_remote_build.sh",
)


# What a release must show beside the finished package: the full replay76 receipt and the fresh-user receipts of the exact
# player and Debug archives. The manifest holds their SHA-256 (the files stay where they were written).
RELEASE_EVIDENCE = ("replay76", "fresh_user_player", "fresh_user_debug")
RELEASE_OVERLAY = {"tools/release/kit-label.txt", "config/launcher-pin.json"}
MODES = ("kit", "release")


class ManifestError(ValueError):
    pass


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def digest(value) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git(repo: Path, *args: str, check: bool = True) -> bytes:
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=False)
    if check and result.returncode:
        raise ManifestError(f"git {' '.join(args[:2])} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout if result.returncode == 0 else b""


def blob_entry(repo: Path, commit: str, path: str, *, required: bool = True):
    """{path, git_blob, sha256} of one file at a commit, or None when an optional file is absent."""
    listed = git(repo, "ls-tree", commit, "--", path, check=False).decode().split()
    if len(listed) < 3 or listed[1] != "blob":
        if required:
            raise ManifestError(f"{path} is not in kit commit {commit[:12]}")
        return None
    return {"path": path, "git_blob": listed[2], "sha256": sha256_bytes(git(repo, "cat-file", "blob", listed[2]))}


def extract_payload(repo: Path, commit: str, destination: Path) -> Path:
    """Extract the files finish runs and reads from the kit commit itself, so no check uses the operator's copies."""
    import io
    import tarfile
    names = [name for name in PAYLOAD_TREES if git(repo, "ls-tree", commit, "--", name, check=False).strip()]
    archive = git(repo, "archive", "--format=tar", commit, "--", *names)
    destination = Path(destination)
    if destination.exists():  # a finish that stopped on a failed check is run again
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            parts = Path(member.name).parts
            if member.name.startswith(("/", "\\")) or ".." in parts or not (member.isfile() or member.isdir()):
                raise ManifestError(f"unsafe payload entry: {member.name}")
        try:
            tar.extractall(destination, filter="data")
        except TypeError:  # Python before the extraction filters
            tar.extractall(destination)
    return destination


def blob_id_of_file(repo: Path, repo_path: str, file: Path) -> str:
    """The git blob id a working-tree file has as `repo_path`: the repository's own line-ending rules are applied, so a
    CRLF checkout (core.autocrlf) of a file hashes like the LF blob the commit stores."""
    result = subprocess.run(["git", "-C", str(repo), "hash-object", "--stdin", "--path", repo_path],
                            input=Path(file).read_bytes(), capture_output=True, check=False)
    if result.returncode:
        raise ManifestError(f"git hash-object failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout.decode().strip()


def orchestrator(repo: Path) -> dict:
    """The operator's checkout that runs the cut: its commit, whether the orchestrating files are clean, their hashes."""
    head = git(repo, "rev-parse", "HEAD").decode().strip()
    dirty = git(repo, "status", "--porcelain", "--untracked-files=no", "--", *ORCHESTRATOR_FILES).decode().strip()
    files = []
    for name in ORCHESTRATOR_FILES:
        path = Path(repo) / name
        if path.is_file():
            files.append({"path": name, "sha256": sha256_file(path)})
    return {"tooling_commit": head, "clean": not dirty, "files": files}


def build_plan(repo: Path, *, base: str, kit: str, label: str, kit_version: str, overlay: list[str],
               launcher_pin: dict, engine: tuple[str, dict], changes_sha256: str, route: dict,
               operator: dict | None = None) -> dict:
    """The plan section for a kit commit. Everything in it is derived from git objects or the arguments."""
    for name, value in (("base", base), ("kit", kit)):
        if not HEX40.fullmatch(value):
            raise ManifestError(f"{name} commit must be a full lowercase hash")
    payload = [blob_entry(repo, kit, name) for name in PAYLOAD_REQUIRED]
    payload += [entry for name in PAYLOAD_OPTIONAL if (entry := blob_entry(repo, kit, name, required=False))]
    engine_label, engine_files = engine
    release_version = None
    entry = next((e for e in payload if e["path"] == "config/release-version.json"), None)
    if entry is not None:
        release_version = json.loads(git(repo, "cat-file", "blob", entry["git_blob"]))["version"]
    plan = {
        "version": {"release_version": release_version, "label": label, "kit_version": kit_version},
        "source": {"base_sha": base, "base_tree": git(repo, "rev-parse", base + "^{tree}").decode().strip(),
                   "kit_sha": kit, "kit_tree": git(repo, "rev-parse", kit + "^{tree}").decode().strip(),
                   "overlay": [blob_entry(repo, kit, name) for name in overlay]},
        "payload": payload,
        "inputs": {"engine": {"label": engine_label, "files": dict(sorted(engine_files.items()))},
                   "launcher": {"version": launcher_pin["launcher_version"], "file": launcher_pin["artifact"]["file"],
                                "sha256": launcher_pin["artifact"]["sha256"]},
                   "revised_layer": {"manifest_sha256": next(e["sha256"] for e in payload
                                                             if e["path"] == "tools/release/revised-layer-manifest.json")},
                   "readme_template_sha256": next(e["sha256"] for e in payload
                                                  if e["path"] == "tools/release/kit-readme.template.md"),
                   "changes_sha256": changes_sha256},
        "orchestrator": operator if operator is not None else orchestrator(repo),
        "route": route,
    }
    return plan


def new_manifest(plan: dict, mode: str = "kit") -> dict:
    if mode not in MODES:
        raise ManifestError(f"mode must be one of {MODES}")
    return {"schema": SCHEMA, "mode": mode, "plan": plan, "plan_sha256": digest(plan)}


def default_verify_tag(repo: Path, tag: str) -> bool:
    return subprocess.run(["git", "-C", str(repo), "verify-tag", tag], capture_output=True).returncode == 0


def tag_facts(repo: Path, tag: str, verify=default_verify_tag) -> dict:
    """What a release records about its tag: an annotated tag object whose signature this checkout verifies."""
    ref = f"refs/tags/{tag}"
    try:
        kind = git(repo, "cat-file", "-t", ref).decode().strip()
    except ManifestError as error:
        raise ManifestError(f"tag {tag} does not exist in this checkout") from error
    if kind != "tag":
        raise ManifestError(f"{tag} is a lightweight tag; a release needs an annotated, signed tag object")
    if not verify(repo, tag):
        raise ManifestError(f"git cannot verify the signature of {tag}; tag it with `git tag -s` and use a checkout that trusts the key")
    return {"tag": tag, "tag_object": git(repo, "rev-parse", ref).decode().strip(),
            "tag_commit": git(repo, "rev-parse", ref + "^{commit}").decode().strip(), "signature": "good"}


def add_host(manifest: dict, host: dict) -> dict:
    section = dict(host, plan_sha256=manifest["plan_sha256"])
    manifest["host"] = section
    manifest["host_sha256"] = digest(section)
    return manifest


def add_evidence(manifest: dict, items: dict) -> dict:
    """Release evidence, bound to the plan and the finish section (so to the delivered archive)."""
    if manifest.get("mode") != "release" or "finish" not in manifest:
        raise ManifestError("evidence belongs to a finished release manifest")
    section = {"plan_sha256": manifest["plan_sha256"], "finish_sha256": manifest["finish_sha256"], "items": items}
    manifest["evidence"] = section
    manifest["evidence_sha256"] = digest(section)
    return manifest


def add_finish(manifest: dict, finish: dict) -> dict:
    section = dict(finish, plan_sha256=manifest["plan_sha256"], host_sha256=manifest["host_sha256"])
    manifest["finish"] = section
    manifest["finish_sha256"] = digest(section)
    return manifest


def write(path: Path, manifest: dict) -> None:
    Path(path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def read(path: Path) -> dict:
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise ManifestError(f"{Path(path).name} is not a {SCHEMA} file")
    return data


def player_output(manifest: dict) -> dict:
    outputs = [o for o in manifest.get("host", {}).get("outputs", []) if o.get("name", "").endswith("-player.zip")]
    if len(outputs) != 1:
        raise ManifestError("the host section must list exactly one player archive")
    return outputs[0]


def _shape(plan) -> list[str]:
    problems = []
    try:
        for section in ("version", "source", "payload", "inputs", "orchestrator", "route"):
            if section not in plan:
                problems.append(f"plan has no {section}")
        for key in ("base_sha", "kit_sha"):
            if not HEX40.fullmatch(plan["source"][key]):
                problems.append(f"plan source {key} is not a full hash")
        for entry in plan["payload"] + plan["source"]["overlay"]:
            if not (HEX40.fullmatch(entry["git_blob"]) and HEX64.fullmatch(entry["sha256"])):
                problems.append(f"plan entry {entry.get('path')} has a malformed hash")
    except (KeyError, TypeError) as error:
        problems.append(f"plan is malformed: {error!r}")
    return problems


def verify_sums_folder(folder: Path, sums_name: str = "SHA256SUMS") -> tuple[list[str], str]:
    """Problems of a kit folder against its own SHA256SUMS, and the SHA-256 of that sums file."""
    folder = Path(folder)
    sums = folder / sums_name
    if not sums.is_file():
        return [f"{folder.name} has no {sums_name}"], ""
    listed = {}
    for line in sums.read_text(encoding="utf-8").splitlines():
        value, _, name = line.partition("  ")
        listed[name] = value
    actual = {p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file()} - {sums_name}
    problems = [f"{name}: not listed in {sums_name}" for name in sorted(actual - set(listed))]
    problems += [f"{name}: listed but missing" for name in sorted(set(listed) - actual)]
    problems += [f"{name}: SHA-256 differs" for name in sorted(actual & set(listed))
                 if sha256_file(folder / name) != listed[name]]
    return problems, sha256_file(sums)


def receipt_problems(manifest: dict, host: dict) -> list[str]:
    """The host receipt (host_package.py) agrees with the plan and with the outputs the host section lists."""
    receipt = host.get("receipt")
    if receipt is None:
        return []
    plan = manifest["plan"]
    problems = []
    if not isinstance(receipt, dict) or receipt.get("schema") != "eawr-host-receipt/1":
        return ["the host receipt has the wrong schema"]
    if receipt.get("plan_sha256") != manifest["plan_sha256"]:
        problems.append("the host receipt was written against another plan")
    if receipt.get("kit_sha") != plan["source"]["kit_sha"]:
        problems.append("the host receipt is for another kit commit")
    if receipt.get("tree", {}).get("tracked_clean") is not True or receipt.get("tree", {}).get("head") != plan["source"]["kit_sha"]:
        problems.append("the host receipt does not show a clean tree at the kit commit")
    listed = {(o.get("name"), o.get("sha256"), o.get("size")) for o in receipt.get("outputs", [])}
    for output in host.get("outputs", []):
        if (output.get("name"), output.get("sha256"), output.get("size")) not in listed:
            problems.append(f"{output.get('name')} is not among the outputs the host receipt hashed")
    if isinstance(plan.get("release"), dict) and receipt.get("public") is not True:
        problems.append("the host receipt does not show a --public package, which a release requires")
    problems += _receipt_input_problems(receipt, plan)
    problems += _receipt_toolchain_problems(receipt.get("toolchain"), plan.get("route", {}).get("kind"))
    return problems


def _release_problems(manifest: dict, require_evidence: bool) -> list[str]:
    """A release is a kit with more to prove: a signed annotated tag on the base commit, nothing added to the tagged tree but the
    label and pin, a clean orchestrator, a reviewed layer, no waivers, an unchanged package, and the evidence."""
    plan = manifest["plan"]
    problems = []
    release = plan.get("release")
    if not isinstance(release, dict):
        return ["a release manifest records no release section in its plan"]
    for key in ("tag_object", "tag_commit"):
        if not HEX40.fullmatch(str(release.get(key, ""))):
            problems.append(f"release {key} is not a full hash")
    version = plan["version"]
    if release.get("signature") != "good":
        problems.append("the release tag's signature was not verified")
    if not version.get("release_version") or release.get("tag") != "v" + str(version.get("release_version")):
        problems.append("the release tag is not v<release version> of config/release-version.json")
    if version.get("label") != version.get("release_version"):
        problems.append("a release carries the release version as its label, with no suffix")
    if release.get("tag_commit") != plan["source"]["base_sha"]:
        problems.append("the release tag does not point at the commit the package was cut from")
    if not plan["orchestrator"].get("clean"):
        problems.append("a release is cut with clean orchestrating scripts")
    extra = sorted({e["path"] for e in plan["source"]["overlay"]} - RELEASE_OVERLAY)
    if extra:
        problems.append(f"a release adds nothing to the tagged commit but the label and pin, not {extra}")
    if release.get("public") is not True:
        problems.append("a release is packaged with --public (the exact reviewed Revised layer and release symbols)")
    finish, host = manifest.get("finish"), manifest.get("host")
    if finish is not None:
        if finish.get("layer", {}).get("mode") != "reviewed":
            problems.append("a release ships exactly the reviewed Revised layer")
        allow = finish.get("allow", {})
        if allow.get("allow_diff") or allow.get("allow_binary_diff"):
            problems.append("a release is judged without --allow-diff or --allow-binary-diff waivers")
        if host is not None:
            try:
                if finish.get("delivered", {}).get("sha256") != player_output(manifest)["sha256"]:
                    problems.append("a release delivers the host's package unchanged, but the delivered archive differs from it")
            except ManifestError:
                pass
    evidence = manifest.get("evidence")
    if evidence is None:
        if require_evidence:
            problems.append("a release manifest needs its evidence section (replay76 and fresh-user receipts)")
        return problems
    if finish is None:
        return problems + ["evidence without a finish section"]
    if evidence.get("plan_sha256") != manifest["plan_sha256"] or evidence.get("finish_sha256") != manifest.get("finish_sha256"):
        problems.append("the evidence was written against another plan or finish section")
    if digest(evidence) != manifest.get("evidence_sha256"):
        problems.append("evidence_sha256 does not match the evidence section")
    items = evidence.get("items", {})
    for name in RELEASE_EVIDENCE:
        item = items.get(name)
        if name == "fresh_user_debug" and isinstance(item, dict) and item == {"shipped": False}:
            continue  # a release that ships no Debug archive says so explicitly (release_evidence.py add --no-debug-archive)
        if not isinstance(item, dict) or not HEX64.fullmatch(str(item.get("sha256", ""))):
            problems.append(f"the evidence has no {name} receipt hash")
    player = items.get("fresh_user_player", {})
    if player and player.get("archive_sha256") != finish.get("delivered", {}).get("sha256"):
        problems.append("the fresh-user player receipt is not for the delivered archive")
    # The Debug archive is not an output of this manifest: it comes from a separate Debug build and package (`--kit-debug`),
    # so the manifest cannot compare its hash with a host output. It binds the Debug fresh-user receipt by the archive hash that
    # receipt names, and the maintainer uploads exactly that archive (docs/releasing.md: the fresh-user run is on the archive
    # that is uploaded).
    debug = items.get("fresh_user_debug", {})
    if debug and debug != {"shipped": False} and not HEX64.fullmatch(str(debug.get("archive_sha256", ""))):
        problems.append("the fresh-user Debug receipt names no archive hash")
    return problems


def _receipt_input_problems(receipt: dict, plan: dict) -> list[str]:
    """The inputs the host says it verified are the ones the plan pinned: the engine files and the launcher artifact."""
    inputs = receipt.get("inputs")
    if not isinstance(inputs, dict):
        return ["the host receipt lists no verified inputs"]
    problems = []
    pinned = plan.get("inputs", {})
    engine = pinned.get("engine", {}).get("files", {})
    if not engine:
        problems.append("the plan pins no engine files to compare the host receipt with")
    for name, want in sorted(engine.items()):
        if inputs.get(name) != want:
            problems.append(f"the host receipt's {name} is not the engine file the plan pinned")
    if inputs.get("launcher.zip") != pinned.get("launcher", {}).get("sha256"):
        problems.append("the host receipt's launcher.zip is not the artifact the plan pinned")
    return problems


def _receipt_toolchain_problems(toolchain, route_kind) -> list[str]:
    """A kit cut records the compiler and linker that built it, and the tools of the route it ran on."""
    if not isinstance(toolchain, dict):
        return ["the host receipt records no toolchain"]
    problems = [f"the host receipt records no {key}" for key in ("clang", "lld") if not toolchain.get(key)]
    host_os = str(toolchain.get("os") or "")
    if route_kind == "linux-xbuild":
        if not host_os.startswith("Linux"):
            problems.append(f"the plan's route is the Linux cross host, the receipt's host is {host_os or 'unknown'}")
        if not toolchain.get("wine"):
            problems.append("the host receipt records no Wine (the Linux route runs the engine through it)")
    elif route_kind == "windows-pool":
        if not host_os.startswith("Windows"):
            problems.append(f"the plan's route is the Windows pool, the receipt's host is {host_os or 'unknown'}")
        if not toolchain.get("powershell"):
            problems.append("the host receipt records no PowerShell (the pool route runs under it)")
    else:
        problems.append(f"the plan's route {route_kind!r} is neither linux-xbuild nor windows-pool")
    return problems


def verify(manifest: dict, *, zip_path: Path | None = None, folder: Path | None = None,
           repo: Path | None = None, require_host: bool = False, require_finish: bool = False,
           expect_base: str | None = None, require_release: bool = False) -> list[str]:
    """Every problem found, empty when the manifest and what it describes agree."""
    problems = []
    plan = manifest.get("plan")
    if manifest.get("schema") != SCHEMA:
        return [f"schema is not {SCHEMA}"]
    problems += _shape(plan or {})
    if problems:
        return problems
    if digest(plan) != manifest.get("plan_sha256"):
        problems.append("plan_sha256 does not match the plan")
    if manifest.get("mode") not in MODES:
        problems.append(f"mode must be one of {MODES}")
    elif manifest["mode"] == "kit" and (plan.get("release") is not None or "evidence" in manifest):
        problems.append("a kit manifest carries no release section or evidence")
    elif require_release and manifest["mode"] != "release":
        problems.append("this is a kit manifest, not a release manifest")
    if manifest.get("mode") == "release" or require_release:
        problems += _release_problems(manifest, require_release)
    host, finish = manifest.get("host"), manifest.get("finish")
    if require_host and host is None:
        problems.append("the manifest has no host section")
    if require_finish and finish is None:
        problems.append("the manifest has no finish section")
    if host is not None:
        if host.get("plan_sha256") != manifest["plan_sha256"]:
            problems.append("host section was written against another plan")
        if digest(host) != manifest.get("host_sha256"):
            problems.append("host_sha256 does not match the host section")
        if not isinstance(host.get("host"), str) or not host["host"]:
            problems.append("the host section names no host")
        try:
            player_output(manifest)
        except ManifestError as error:
            problems.append(str(error))
        problems += receipt_problems(manifest, host)
    if finish is not None:
        if host is None:
            problems.append("finish section without a host section")
        elif finish.get("plan_sha256") != manifest["plan_sha256"] or finish.get("host_sha256") != manifest.get("host_sha256"):
            problems.append("finish section was written against another plan or host section")
        if digest(finish) != manifest.get("finish_sha256"):
            problems.append("finish_sha256 does not match the finish section")
    if zip_path is not None:
        zip_path = Path(zip_path)
        if finish is not None:
            want = finish.get("delivered", {})
            label = "delivered archive"
        elif host is not None and not problems:
            want = player_output(manifest)
            label = "player archive"
        else:
            want, label = {}, "archive"
        if not zip_path.is_file():
            problems.append(f"{label} is missing: {zip_path}")
        elif not want:
            problems.append(f"no recorded {label} to compare with")
        else:
            if sha256_file(zip_path) != want.get("sha256"):
                problems.append(f"{label} SHA-256 differs from the manifest")
            if zip_path.stat().st_size != want.get("size"):
                problems.append(f"{label} size differs from the manifest")
    if folder is not None:
        found, sums_hash = verify_sums_folder(Path(folder))
        problems += found
        if finish is not None and sums_hash and sums_hash != finish.get("delivered", {}).get("sums_sha256"):
            problems.append("the kit's SHA256SUMS differs from the manifest")
    if expect_base is not None and plan["source"]["base_sha"] != expect_base:
        problems.append(f"the plan's base commit is {plan['source']['base_sha'][:12]}, expected {expect_base[:12]}")
    if repo is not None:
        source = plan["source"]
        try:
            if git(repo, "rev-parse", source["kit_sha"] + "^{tree}").decode().strip() != source["kit_tree"]:
                problems.append("kit commit tree differs from the plan")
            if git(repo, "rev-parse", source["kit_sha"] + "^").decode().strip() != source["base_sha"]:
                problems.append("the kit commit is not a child of the plan's base commit")
            changed = set(git(repo, "diff-tree", "-r", "--name-only", "--no-commit-id", source["base_sha"],
                              source["kit_sha"]).decode().split("\n")) - {""}
            overlay = {e["path"] for e in source["overlay"]}
            if changed - overlay:  # an overlay file may equal the base's (the same pin): only extra changes are wrong
                problems.append(f"the kit commit changes {sorted(changed - overlay)[:5]} beyond the plan's overlay")
            for entry in plan["payload"] + source["overlay"]:
                found = blob_entry(repo, source["kit_sha"], entry["path"])
                if found != entry:
                    problems.append(f"{entry['path']} differs from the kit commit")
        except ManifestError as error:
            problems.append(str(error))
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("verify", help="check a manifest and, optionally, the kit it describes")
    check.add_argument("manifest", type=Path)
    check.add_argument("--zip", type=Path)
    check.add_argument("--folder", type=Path)
    check.add_argument("--repo", type=Path, help="a checkout that holds the kit commit")
    check.add_argument("--require-finish", action="store_true")
    check.add_argument("--release", action="store_true",
                       help="require a finished release manifest with its tag, public package and evidence")
    check.add_argument("--expect-base", help="a commit or ref the kit must have been cut from (needs --repo)")
    show = sub.add_parser("show", help="print the manifest's summary")
    show.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = read(args.manifest)
    except (OSError, ValueError) as error:
        print(f"kit_manifest: {error}", file=sys.stderr)
        return 2
    if args.command == "show":
        plan = manifest["plan"]
        print(f"kit {plan['version']['kit_version']} from {plan['source']['kit_sha']} (base {plan['source']['base_sha']})")
        print(f"plan {manifest['plan_sha256']}; host {'yes' if 'host' in manifest else 'no'}; "
              f"finish {'yes' if 'finish' in manifest else 'no'}")
        return 0
    expect = None
    if args.expect_base:
        if args.repo is None:
            print("kit_manifest: --expect-base needs --repo", file=sys.stderr)
            return 2
        try:
            expect = git(args.repo, "rev-parse", args.expect_base + "^{commit}").decode().strip()
        except ManifestError as error:
            print(f"kit_manifest: {error}", file=sys.stderr)
            return 2
    problems = verify(manifest, zip_path=args.zip, folder=args.folder, repo=args.repo,
                      require_host=True, require_finish=args.require_finish or args.release, expect_base=expect,
                      require_release=args.release)
    for problem in problems:
        print(f"FAIL {problem}")
    print("manifest ok" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
