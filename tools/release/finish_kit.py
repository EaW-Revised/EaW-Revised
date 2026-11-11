"""Finish a cut kit: verify the fetched player zip, add the Revised layer, write README, deliver folder + zip + .sha256.

  python finish_kit.py --work <work dir of cut_kit> --out-root out/owner-kit [--reference <player folder>]... [--allow-diff GLOB]...

Checks (any failure = exit 4, nothing delivered; the report is printed and written to <work>/finish-report.txt):
  engine      tools/release/kit_engine.py verify: every exe under godot/ is the pinned PATCHED engine (#5826)
  identity    every embedded '<ver>+<40 hex>' build identity in the exes/dlls equals the kit commit
  pdb         every own exe/dll (not godot/, launcher/) has a CodeView PDB link (was linked with symbols)
  pin         launcher-pin.json == the pin the kit commit carries; launcher artifact sha256 == pin
  layout      file list against each --reference player folder; only --allow-diff patterns (and the built-in version files) may differ
  layer       Revised layer present and equal to the reviewed layer manifest of the kit commit; SHA256SUMS rewritten by
              the layer step is verified file by file

Everything is judged by the kit commit, not by this checkout: the cut's kit-manifest.json is verified first (the plan
hash, the player archive against the host record, the git objects the plan names), and the engine pins, the README
template, the launcher pin and the packager modules are taken from `git archive` of the kit commit. The finished kit gets
a sidecar manifest (<name>.zip.manifest.json) with the finish section; the kit folder itself is not changed by it.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kit_manifest  # noqa: E402  (this file's own copy only reads and writes the manifest)
ROOT = Path(__file__).resolve().parents[2]

# Files whose bytes (not name) carry the version, or that every kit rewrites: never reported as layout differences.
BUILTIN_ALLOW = ["EAWR (Safe render thread).cmd", "README.md",  # the Safe shortcut is gone since launcher 0.1.15
                  "SHA256SUMS", "launcher-pin.json"]
# Pinned inputs: the engine and the launcher project must be byte-equal to the reference kit's, whatever the allow lists say,
# unless --allow-binary-diff names them. Own binaries (exe/dll outside these folders) are rebuilt for every kit.
PINNED_DIRS = ("godot/", "launcher/")
PINNED_SUFFIXES = (".pck", ".so", ".dylib")
# A name pattern that could hide a binary is refused by --allow-diff: these probes must not match.
BINARY_PROBES = ("a.exe", "dir/a.exe", "a.dll", "project/bin/a.dll", "a.pck", "a.so", "godot/a", "godot/sub/a.exe", "launcher/a",
                 "launcher/sub/a.dll")
ID_RE = re.compile(rb"\d+\.\d+\.\d+[0-9A-Za-z.-]*\+([0-9a-f]{40})")
# Linked without a PDB link in every kit so far (0.3.2n loadcheck too): reported, not a failure. Everything else must link one.
# Rebuilt for every kit but embed no build identity (the launcher stubs and the Bink helper), so only their names can be pinned.
IDENTITYLESS = {"EAWR.exe", "EAWR Mods.exe", "project/common/eawr_bink_helper.exe"}
PDB_WARN_ONLY = {"eawr_loadcheck.exe", "eawr_mods_service.exe", "project/common/eawr_bink_helper.exe"}


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def pdb_link(path):
    """CodeView PDB path of a PE file: None = not a PE, '' = no CodeView entry."""
    d = Path(path).read_bytes()
    if d[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        return None
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    osz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    rva, size = struct.unpack_from("<II", d, opt + (112 if magic == 0x20B else 96) + 6 * 8)
    if not rva:
        return ""
    secs = [struct.unpack_from("<8sIIII", d, opt + osz + i * 40)[1:] for i in range(nsec)]

    def off(r):
        for vs, va, rs, ro in secs:
            if va <= r < va + max(vs, 1):
                return r - va + ro
    o = off(rva)
    for i in range(size // 28):
        b = o + i * 28
        typ = struct.unpack_from("<I", d, b + 12)[0]
        sz = struct.unpack_from("<I", d, b + 16)[0]
        ptr = struct.unpack_from("<I", d, b + 24)[0]
        if typ == 2 and d[ptr:ptr + 4] == b"RSDS":
            return d[ptr + 24:ptr + sz].split(b"\0")[0].decode(errors="replace")
    return ""


def tree(root):
    return {p.relative_to(root).as_posix() for p in Path(root).rglob("*") if p.is_file()}


def extract_player(archive, destination):
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            path = PurePosixPath(member.filename)
            if path.is_absolute() or ".." in path.parts or "\\" in member.filename or ":" in member.filename:
                raise ValueError(f"unsafe archive member: {member.filename}")
        z.extractall(destination)


def refused_patterns(patterns):
    """The --allow-diff patterns that could hide an executable, library, engine or launcher file."""
    return [g for g in patterns if any(fnmatch.fnmatchcase(probe, g) for probe in BINARY_PROBES)]


def protected_name(name):
    """A file whose presence or absence must never be hidden by an allow pattern: executables, libraries, the engine, the
    launcher, the pinned library suffixes and the Revised layer."""
    return (name.lower().endswith((".exe", ".dll")) or pinned_binary(name) or name.startswith(("Revised/", "project/bin/")))


def refused_by_name(patterns, names):
    """The patterns that match a protected file in `names` (the finished kit's and the references' file lists)."""
    protected = [n for n in names if protected_name(n)]
    return [g for g in patterns if any(fnmatch.fnmatchcase(n, g) for n in protected)]


def refused_binary_allowances(patterns):
    """--allow-binary-diff is explicit, but a pattern that matches everything would switch the pinned-byte check off."""
    return [g for g in patterns if g.replace("*", "").replace("?", "").replace(".", "").replace("/", "") == ""]


def pinned_binary(name):
    return name.startswith(PINNED_DIRS) or name.endswith(PINNED_SUFFIXES)


def compare_layout(top, ref, allow, binary_allow=()):
    """Names and bytes of a finished kit against one reference player folder."""
    top, ref = Path(top), Path(ref)
    mine, theirs = tree(top), tree(ref)
    patterns = list(allow) + list(binary_allow)

    def allowed(name):
        return any(fnmatch.fnmatchcase(name, g) for g in patterns)

    added = sorted(f for f in mine - theirs if not allowed(f))
    removed = sorted(f for f in theirs - mine if not allowed(f))
    hidden = sorted(f for f in mine ^ theirs if allowed(f))
    changed = sorted(f for f in mine & theirs if sha256_file(top / f) != sha256_file(ref / f))
    pinned = sorted(f for f in changed if pinned_binary(f) and not any(fnmatch.fnmatchcase(f, g) for g in binary_allow))
    return {"reference": ref.name, "reference_files": len(theirs),
            "reference_list_sha256": kit_manifest.sha256_bytes("\n".join(sorted(theirs)).encode("utf-8")),
            "added": added, "removed": removed, "allowed_differences": hidden, "changed_content": changed,
            "changed_binaries": [f for f in changed if f.lower().endswith((".exe", ".dll"))],
            "pinned_binaries_changed": pinned}


def describe_identityless(top):
    """For each named identity-less file: its SHA-256 and whether it embeds a build identity (it must not: that is why only its
    name can be pinned). Recorded in the manifest as the evidence for the exemption."""
    found = {}
    for name in sorted(IDENTITYLESS):
        path = Path(top) / name
        if path.is_file():
            data = path.read_bytes()
            found[name] = {"sha256": hashlib.sha256(data).hexdigest(),
                           "embedded_identities": sorted({m.decode() for m in ID_RE.findall(data)})}
    return found


def unexplained_binaries(changed, carriers):
    """Changed exe/dll files that are neither pinned inputs (checked separately), nor carry the kit's identity, nor are named."""
    return [f for f in changed if not pinned_binary(f) and f not in carriers and f not in PDB_WARN_ONLY and f not in IDENTITYLESS]


def check_launcher(kit_launcher, artifact, expected_sha256):
    """Problems of the kit's launcher/ folder against the pinned artifact zip: same names and same bytes."""
    problems = []
    if not Path(artifact).is_file():
        return [f"launcher artifact is missing: {artifact}"]
    if sha256_file(artifact) != expected_sha256:
        return ["launcher artifact differs from the plan's pinned SHA-256"]
    with zipfile.ZipFile(artifact) as z:
        expected = {PurePosixPath(m.filename).as_posix(): hashlib.sha256(z.read(m)).hexdigest()
                    for m in z.infolist() if not m.is_dir()}
    actual = {p.relative_to(kit_launcher).as_posix(): sha256_file(p) for p in Path(kit_launcher).rglob("*") if p.is_file()}
    problems += [f"launcher/{n}: not in the pinned artifact" for n in sorted(set(actual) - set(expected))]
    problems += [f"launcher/{n}: missing from the kit" for n in sorted(set(expected) - set(actual))]
    problems += [f"launcher/{n}: bytes differ from the pinned artifact" for n in sorted(set(actual) & set(expected))
                 if actual[n] != expected[n]]
    return problems


def check_layer(layer, reviewed_manifest, plan, allow_unreviewed):
    """Compare the layer directory with the reviewed manifest of the kit commit: names, sizes and SHA-256."""
    reviewed = json.loads(Path(reviewed_manifest).read_text(encoding="utf-8"))
    expected = {name: (entry["size"], entry["sha256"]) for name, entry in reviewed["files"].items()}
    layer = Path(layer)
    actual = {p.relative_to(layer).as_posix(): (p.stat().st_size, sha256_file(p)) for p in layer.rglob("*") if p.is_file()} \
        if layer.is_dir() else {}
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    changed = sorted(n for n in set(actual) & set(expected) if actual[n] != expected[n])
    state = {"mode": "reviewed" if not (missing or extra or changed) else "unreviewed",
             "reviewed_manifest_sha256": plan["inputs"]["revised_layer"]["manifest_sha256"], "problems": []}
    if state["mode"] == "unreviewed":
        state["differences"] = {"missing": len(missing), "extra": len(extra), "changed": len(changed)}
        if not allow_unreviewed:
            state["problems"].append(
                f"the layer differs from the reviewed manifest (missing {missing[:3]}, extra {extra[:3]}, changed {changed[:3]}; "
                f"{len(missing)}/{len(extra)}/{len(changed)} in total); pass --allow-unreviewed-layer to ship it anyway")
    return state


def write_reproducible_zip(archive, folder, date_time):
    names = sorted(p for p in folder.rglob("*") if p.is_file() and p.name != "SHA256SUMS") + [folder / "SHA256SUMS"]
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in names:
            info = zipfile.ZipInfo(f"{folder.name}/{path.relative_to(folder).as_posix()}", date_time=date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o755 if path.stat().st_mode & 0o111 else 0o644) << 16
            z.writestr(info, path.read_bytes())


def zip_time(repo, commit):
    """The kit commit's committer time as a zip entry time (zip cannot hold dates before 1980)."""
    stamp = int(kit_manifest.git(repo, "show", "-s", "--format=%ct", commit).decode().strip())
    moment = time.gmtime(max(stamp, 315532800))
    return (moment.tm_year, moment.tm_mon, moment.tm_mday, moment.tm_hour, moment.tm_min, moment.tm_sec - moment.tm_sec % 2)


def delivery_paths(out, name):
    dest, zpath, hashpath = out / name, out / (name + '.zip'), out / (name + '.zip.sha256')
    sidecar = out / (name + kit_manifest.SIDECAR_SUFFIX)
    for existing in (dest, zpath, hashpath, sidecar):
        if existing.exists() or existing.is_symlink():
            raise SystemExit(f"{existing} already exists; refusing to overwrite")
    return dest, zpath, hashpath, sidecar


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--out-root", required=True, type=Path)
    ap.add_argument("--reference", action="append", type=Path, required=True, help="accepted player folder to compare")
    ap.add_argument("--layer", type=Path, help="reviewed Revised layer directory (a kit; a release already holds it)")
    ap.add_argument("--allow-unreviewed-layer", action="store_true",
                    help="ship a --layer that differs from the kit commit's revised-layer-manifest.json; recorded in the manifest")
    ap.add_argument("--allow-diff", action="append", default=[], metavar="GLOB",
                    help="extra file-name differences that are expected (patterns that could hide an exe, dll, engine or launcher "
                         "file are refused; recorded in the manifest)")
    ap.add_argument("--allow-binary-diff", action="append", default=[], metavar="GLOB",
                    help="a pinned or binary file that may differ from the reference; recorded in the manifest")
    ap.add_argument("--expect-base", metavar="REF", help="refuse unless the kit was cut from this commit or ref")
    ap.add_argument("--repo", default=str(ROOT))
    a = ap.parse_args()
    meta = json.loads((a.work / "cut.json").read_text(encoding="utf-8"))
    if not meta.get("ok"):
        raise SystemExit("cut.json says the cut did not succeed")
    problems, lines = [], []

    def say(m):
        lines.append(m)
        print(m, flush=True)

    def fail(m):
        problems.append(m)
        say("FAIL " + m)

    repo = Path(a.repo)
    for bad in refused_patterns(a.allow_diff):
        fail(f"allow-diff: {bad!r} could hide an executable, library, engine or launcher file; use --allow-binary-diff for that")
    try:
        manifest = kit_manifest.read(a.work / kit_manifest.MANIFEST_NAME)
        plan = manifest["plan"]
        release = manifest.get("mode") == "release"
        kit_sha, label, base_sha = plan["source"]["kit_sha"], plan["version"]["label"], plan["source"]["base_sha"]
        expect_base = kit_manifest.git(repo, "rev-parse", a.expect_base + "^{commit}").decode().strip() if a.expect_base else None
    except (OSError, ValueError, KeyError) as error:
        fail(f"manifest: cannot use {kit_manifest.MANIFEST_NAME}: {error}")
        (a.work / "finish-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        say(f"{len(problems)} problem(s); nothing extracted or delivered. Report: {a.work / 'finish-report.txt'}")
        return 4
    if release:
        for flag, given in (("--layer", a.layer), ("--allow-unreviewed-layer", a.allow_unreviewed_layer),
                            ("--allow-diff", a.allow_diff), ("--allow-binary-diff", a.allow_binary_diff)):
            if given:
                fail(f"release: {flag} is not accepted for a release package (the package holds its reviewed layer and is judged without waivers)")
    elif a.layer is None:
        fail("--layer is required for a kit")
    # This checkout's finish and verifier must be the kit commit's own bytes, or nothing below can be trusted.
    committed = {e["path"].rsplit("/", 1)[-1]: e["git_blob"] for e in plan.get("payload", [])}
    finish_tool = {}
    for name in ("finish_kit.py", "kit_manifest.py"):
        blob = kit_manifest.blob_id_of_file(repo, "tools/release/" + name, Path(__file__).with_name(name))
        finish_tool[name] = {"git_blob": blob}
        if committed.get(name) != blob:
            fail(f"tool: {name} in this checkout is not the kit commit's copy; run it from "
                 f"`git archive {plan['source']['kit_sha'][:12]} tools/release`")
    if not problems:
        for problem in kit_manifest.verify(manifest, zip_path=Path(meta["player_zip"]), repo=repo, require_host=True,
                                           expect_base=expect_base):
            fail("manifest: " + problem)
    if meta.get("plan_sha256") != manifest["plan_sha256"] or meta.get("kit_sha") != kit_sha:
        fail("manifest: cut.json names another plan or kit commit")
    changes_bytes = Path(meta["changes"]).read_bytes()
    if kit_manifest.sha256_bytes(changes_bytes) != plan["inputs"]["changes_sha256"]:
        fail("changes: the changes file differs from the one the cut recorded")
    if problems:
        (a.work / "finish-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        say(f"{len(problems)} problem(s); nothing extracted or delivered. Report: {a.work / 'finish-report.txt'}")
        return 4
    say(f"manifest ok: plan {manifest['plan_sha256'][:12]}, kit commit {kit_sha[:12]}, player archive matches the host record")

    # The checks run from the kit commit's own files (engine pins, README template, packager modules), not this checkout's.
    payload = kit_manifest.extract_payload(repo, kit_sha, a.work / f"payload-{kit_sha[:8]}")
    sys.path.insert(0, str(payload / "tools/release"))
    import kit_engine
    import add_revised_layer as arl
    import package_release as pr
    x = Path(tempfile.mkdtemp(prefix='finish-', dir=a.work))
    extract_player(meta["player_zip"], x)
    (top,) = [p for p in x.iterdir() if p.is_dir()]
    say(f"extracted {top.name}: {len(tree(top))} files")
    # Name-specific patterns (EAWR.exe, Revised/*) are judged against the real file lists, now that they are known.
    known = set(tree(top))
    for ref in a.reference:
        if ref.is_dir():
            known |= tree(ref)
    for bad in refused_by_name(list(a.allow_diff), known):
        fail(f"allow-diff: {bad!r} matches executables, libraries, engine, launcher or Revised files, which cannot be waved through by name")
    for bad in refused_binary_allowances(a.allow_binary_diff):
        fail(f"allow-binary-diff: {bad!r} matches everything")
    if problems:
        (a.work / "finish-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        say(f"{len(problems)} problem(s); NOT delivered. Report: {a.work / 'finish-report.txt'}")
        return 4

    # engine
    try:
        ids = kit_engine.verify_kit(top)
        say("engine ok: " + ", ".join(f"{i['label']} {Path(i['path']).name}" for i in ids))
    except Exception as e:
        fail(f"engine: {e}")

    # identity + pdb
    seen, own_nopdb, bad_id, carriers = 0, [], [], set()
    for p in sorted(top.rglob("*")):
        if not (p.is_file() and p.suffix.lower() in (".exe", ".dll")):
            continue
        rel = p.relative_to(top).as_posix()
        if rel.startswith(("godot/", "launcher/")):
            continue
        link = pdb_link(p)
        if link is None:
            continue
        if not link:
            own_nopdb.append(rel)
        shas = {m.decode() for m in ID_RE.findall(p.read_bytes())}
        if shas:
            seen += 1
            carriers.add(rel)
        if shas - {kit_sha}:
            bad_id.append(f"{rel}: {sorted(shas)}")
    if bad_id:
        fail("identity: binaries with another build commit: " + "; ".join(bad_id))
    elif not seen:
        fail("identity: no binary carries a build identity")
    else:
        say(f"identity ok: {seen} binaries carry {kit_sha[:12]}, none another")
    hard = [n for n in own_nopdb if n not in PDB_WARN_ONLY]
    if hard:
        fail("pdb: own binaries without a CodeView PDB link: " + ", ".join(hard))
    else:
        say("pdb ok: every own exe/dll has a PDB link" + (f" except the known {sorted(own_nopdb)}" if own_nopdb else ""))

    # pin
    pin_blob = next(e["git_blob"] for e in plan["source"]["overlay"] if e["path"] == "config/launcher-pin.json")
    pin = json.loads(kit_manifest.git(repo, "cat-file", "blob", pin_blob))
    got = json.loads((top / "launcher-pin.json").read_text(encoding="utf-8")) if (top / "launcher-pin.json").is_file() else {}
    if got.get("launcher_version") != pin["launcher_version"] or got.get("artifact", {}).get("sha256") != pin["artifact"]["sha256"] \
            or not (top / "launcher").is_dir():
        fail(f"pin: kit has {got.get('launcher_version')} / {got.get('artifact', {}).get('sha256')}, wanted {pin['launcher_version']}")
    else:
        say(f"pin ok: launcher {pin['launcher_version']} sha256 {pin['artifact']['sha256'][:12]}")

    # launcher/: every file equals the pinned artifact's
    try:
        local = json.loads((a.work / "pkg.json").read_text(encoding="utf-8"))["local_inputs"]["launcher_artifact"]
        launcher_problems = check_launcher(top / "launcher", Path(local), plan["inputs"]["launcher"]["sha256"])
    except (OSError, ValueError, KeyError) as error:
        launcher_problems = [f"cannot find the pinned launcher artifact the cut used: {error}"]
    for problem in launcher_problems:
        fail("launcher: " + problem)
    if not launcher_problems:
        say("launcher ok: launcher/ equals the pinned artifact")

    if release:
        # A release is delivered as the host made it: no README section, no layer added, no archive rebuilt, so the
        # packager's update manifest and sums still describe the delivered bytes. The packaged layer must be the reviewed one.
        pre = Path(meta["player_zip"])
        layer_state = check_layer(top / "Revised", payload / "tools/release/revised-layer-manifest.json", plan, False)
        for problem in layer_state.pop("problems"):
            fail("layer: " + problem)
        if not problems:
            say("layer ok: the packaged Revised layer equals the reviewed manifest")
    else:
        # README (kit section + the packaged guide), then the Revised layer
        changes = changes_bytes.decode("utf-8").strip()
        section = (payload / "tools/release/kit-readme.template.md").read_text(encoding="utf-8")
        extras = [e["path"] for e in plan["source"]["overlay"][2:]]
        for key, val in {"@LABEL@": label, "@SHA8@": kit_sha[:8], "@BASESHA@": base_sha, "@LAUNCHER@": pin["launcher_version"],
                         "@CHANGES@": changes,
                         "@EXTRAS_TEXT@": ("Extra files: " + ", ".join(f"`{e}`" for e in extras) + ".") if extras else ""}.items():
            section = section.replace(key, val)
        readme = top / "README.md"
        readme.write_text(section + readme.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        pre = x / "pre-layer.zip"
        files = sorted(p for p in top.rglob("*") if p.is_file() and p.name != pr.PLAYER_SUMS)
        date_time = zip_time(repo, kit_sha)
        with zipfile.ZipFile(pre, "w", zipfile.ZIP_DEFLATED) as z:
            for p in files:
                z.write(p, f"{top.name}/{p.relative_to(top).as_posix()}")
        layer_state = check_layer(a.layer, payload / "tools/release/revised-layer-manifest.json", plan, a.allow_unreviewed_layer)
        for problem in layer_state.pop("problems"):
            fail("layer: " + problem)
        if layer_state["mode"] == "unreviewed" and not problems:
            say("note: the layer differs from the reviewed manifest and --allow-unreviewed-layer was given")
        arl.add_layer(top, a.layer, pre)
        if not (top / "Revised").is_dir():
            fail("layer: Revised folder missing after add_layer")
        else:
            say("layer ok: Revised added")
            # The delivered archive is rebuilt reproducibly from the finished folder: entry times come from the kit commit, the
            # order and the modes are fixed, so the same kit and layer always give the same zip bytes.
            write_reproducible_zip(pre, top, date_time)
    sums = {}
    for ln in (top / pr.PLAYER_SUMS).read_text(encoding="utf-8").splitlines():
        h, _, n = ln.partition("  ")
        sums[n] = h
    allfiles = tree(top) - {pr.PLAYER_SUMS}
    wrong = [n for n in allfiles if sums.get(n) != sha256_file(top / n)]
    if wrong:
        fail("sums: SHA256SUMS missing/wrong for " + ", ".join(sorted(wrong)[:10]))
    else:
        say(f"sums ok: {len(allfiles)} files")

    # layout diff
    allow = BUILTIN_ALLOW + a.allow_diff
    layouts = []
    for ref in a.reference:
        if not ref.is_dir():
            fail(f"layout: reference {ref} missing")
            continue
        layout = compare_layout(top, ref, allow, a.allow_binary_diff)
        layouts.append(layout)
        say(f"layout vs {ref.name}: {len(tree(top))} vs {layout['reference_files']} files; unexpected added {layout['added']}, "
            f"unexpected removed {layout['removed']}; {len(layout['allowed_differences'])} name difference(s) hidden by allow "
            f"patterns, {len(layout['changed_content'])} file(s) with other bytes "
            f"({len(layout['changed_binaries'])} exe/dll, rebuilt for every kit)")
        if layout["added"] or layout["removed"]:
            fail(f"layout vs {ref.name}: added {layout['added']}, removed {layout['removed']}")
        # A rebuilt exe or dll must be one this kit built: it carries the kit commit's identity or is a known identity-less helper.
        unexplained = unexplained_binaries(layout["changed_binaries"], carriers)
        layout["changed_binaries_without_identity"] = unexplained
        if unexplained:
            fail(f"layout vs {ref.name}: changed executables or libraries that do not carry this kit's build identity: {unexplained}")
        if layout["pinned_binaries_changed"]:
            fail(f"layout vs {ref.name}: pinned engine, launcher or library files differ from the reference: "
                 f"{layout['pinned_binaries_changed']}")

    if not problems:
        digest = sha256_file(pre)
        references = [{"name": ref.name, "sums_sha256": sha256_file(ref / pr.PLAYER_SUMS) if (ref / pr.PLAYER_SUMS).is_file() else None}
                      for ref in a.reference]
        kit_manifest.add_finish(manifest, {
            "checks": [m for m in lines if not m.startswith(("FAIL", "note:"))], "layer": layer_state, "references": references, "layout": layouts,
            "allow": {"builtin": BUILTIN_ALLOW, "allow_diff": list(a.allow_diff), "allow_binary_diff": list(a.allow_binary_diff)},
            "launcher": "equals the pinned artifact",
            "identityless_binaries": describe_identityless(top),
            "finish_tool": finish_tool,
            "delivered": {"name": top.name + ".zip", "size": pre.stat().st_size, "sha256": digest,
                          "sums_sha256": sha256_file(top / pr.PLAYER_SUMS)}})
        for problem in kit_manifest.verify(manifest, zip_path=pre, folder=top, repo=repo, require_finish=True):
            fail("manifest: " + problem)

    (a.work / "finish-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if problems:
        say(f"{len(problems)} problem(s); NOT delivered. Report: {a.work / 'finish-report.txt'}")
        return 4

    # deliver
    out = a.out_root
    out.mkdir(parents=True, exist_ok=True)
    name = top.name
    dest, zpath, hashpath, sidecar = delivery_paths(out, name)
    shutil.copytree(top, dest)
    shutil.copyfile(pre, zpath)
    if release:  # the rest of the package, byte for byte: tools, viewer, the packager's sums and update manifest
        for asset in sorted(Path(meta["player_zip"]).parent.iterdir()):
            if asset.is_file() and asset.name != zpath.name:
                if (out / asset.name).exists():
                    raise SystemExit(f"{out / asset.name} already exists; refusing to overwrite")
                shutil.copyfile(asset, out / asset.name)
    kit_manifest.write(a.work / kit_manifest.MANIFEST_NAME, manifest)
    shutil.copyfile(a.work / kit_manifest.MANIFEST_NAME, sidecar)
    digest = sha256_file(zpath)
    hashpath.write_text(f"{digest}  {zpath.name}\n{sha256_file(sidecar)}  {sidecar.name}\n", encoding="utf-8", newline="\n")
    say(f"delivered {dest}\n          {zpath} {digest} {zpath.stat().st_size} bytes\n          {sidecar}")
    meta.update(delivered=str(dest), zip=str(zpath), zip_sha256=digest, manifest_sidecar=str(sidecar))
    (a.work / "cut.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    (a.work / "finish-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
