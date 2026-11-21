"""tools/release/publish_kit.py: one command that publishes a finished owner kit to the private repository.

`gh` is replaced by FakeGitHub everywhere: no network, no real release and no real tag is ever created."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
sys.path.insert(0, str(ROOT / "tools" / "common"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import kit_manifest  # noqa: E402
import publish_kit  # noqa: E402
import update_manifest  # noqa: E402
from kit_manifest_cases import KitRepo  # noqa: E402

REPO = "owner-x/private-y"
TOP = "eaw-revised-0.3.6c-aaaaaaaa-windows-x64-player"
FILES = {
    "EAWR.exe": b"exe", "EAWR Mods.exe": b"mods", "eawr_mods_service.exe": b"svc", "eawr_loadcheck.exe": b"load",
    "launcher-pin.json": b"{}", "launcher/project.godot": b"l", "godot/Godot.exe": b"g", "project/project.godot": b"p",
    "Revised/Data/XML/a.xml": b"<x/>", "SHA256SUMS": b"sums",
}


class FakeGitHub:
    """Answers the gh calls publish_kit makes from a small model of the repository; records every write."""

    def __init__(self, kit_sha, base_sha, private=True, releases=(), tags=(), commits=None, fail_create=False):
        self.private, self.fail_create = private, fail_create
        self.releases = [{"tag_name": t, "draft": False} for t in releases]
        self.tags = set(tags)
        self.commits = {kit_sha, base_sha} if commits is None else set(commits)
        self.calls, self.writes = [], []
        self.created = None

    def __call__(self, *args):
        self.calls.append(args)
        if args[:2] == ("repo", "view"):
            return json.dumps({"isPrivate": self.private, "visibility": "PRIVATE" if self.private else "PUBLIC", "nameWithOwner": args[2]})
        if args[0] == "api":
            path = next(a for a in args[1:] if a.startswith("repos/"))
            if path.endswith("/releases?per_page=100"):
                return json.dumps(self.releases[:1]) + json.dumps(self.releases[1:])  # two pages, concatenated
            if "/git/matching-refs/tags/" in path:
                return json.dumps([{"ref": "refs/tags/" + t} for t in sorted(self.tags)])
            if "/git/ref/tags/" in path:
                tag = path.split("/git/ref/tags/")[1]
                if tag in self.tags:
                    return "{}"
                raise publish_kit.GhFailed("gh api failed (1): gh: Not Found (HTTP 404)")
            if "/commits/" in path:
                if path.split("/commits/")[1] in self.commits:
                    return "x"
                raise publish_kit.GhFailed("gh api failed (1): No commit found for SHA (HTTP 422)")
            if path.endswith("/releases/latest"):
                raise publish_kit.GhFailed("gh api failed (1): gh: Not Found (HTTP 404)")
            if "/releases/tags/" in path:
                names = [pathlib.Path(a).name for a in self.created[2:5]]
                return json.dumps({"draft": False, "prerelease": False, "assets": [{"name": n} for n in names]})
        if args[:2] == ("release", "create"):
            self.writes.append(args)
            if self.fail_create:
                raise publish_kit.GhFailed("gh release create failed (1): boom")
            self.created = args[1:]
            return f"https://github.com/{REPO}/releases/tag/{args[2]}\n"
        if args[:2] == ("release", "view"):
            return f"https://github.com/{REPO}/releases/tag/{args[2]}\n"
        raise AssertionError(f"unexpected gh call {args}")


class PublishKitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / "repo").mkdir()
        self.repo = KitRepo(self.root / "repo")
        self.out = self.root / "out"
        self.out.mkdir()
        self.zip = self.make_kit()

    def make_kit(self, name=TOP, release_version="0.3.6", mode="kit"):
        zip_path = self.out / (name + ".zip")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for entry, data in FILES.items():
                archive.writestr(f"{name}/{entry}", data)
        digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        plan = self.repo.plan()
        plan["version"]["release_version"] = release_version
        manifest = kit_manifest.new_manifest(plan, mode=mode)
        kit_manifest.add_host(manifest, {"host": "h", "received_from": "log", "outputs": [
            {"name": zip_path.name, "size": zip_path.stat().st_size, "sha256": digest}]})
        kit_manifest.add_finish(manifest, {"delivered": {"name": zip_path.name, "size": zip_path.stat().st_size, "sha256": digest,
                                                         "sums_sha256": "b" * 64}})
        sidecar = self.out / (name + kit_manifest.SIDECAR_SUFFIX)
        kit_manifest.write(sidecar, manifest)
        (self.out / (name + ".zip.sha256")).write_text(f"{digest}  {zip_path.name}\n", encoding="utf-8")
        return zip_path

    def fake(self, **kwargs):
        return FakeGitHub(self.repo.kit, self.repo.base, **kwargs)

    def run_cli(self, fake, *argv):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(publish_kit, "gh", fake), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = publish_kit.main(["--repo", REPO, *argv])
        return code, stdout.getvalue(), stderr.getvalue()

    # --- the happy path ---------------------------------------------------------------------------------------

    def test_publishes_with_tag_target_and_flags(self):
        fake = self.fake()
        notes = self.root / "changes.md"
        notes.write_text("Fixed the thing.\n", encoding="utf-8")
        code, out, err = self.run_cli(fake, "--kit", str(self.zip), "--notes", str(notes))
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(len(fake.writes), 1)
        args = list(fake.writes[0])
        self.assertEqual(args[:3], ["release", "create", "kit-0.3.6-kit.1"])
        self.assertIn("--latest=false", args)
        self.assertNotIn("--prerelease", args)
        self.assertNotIn("--draft", args)
        self.assertEqual(args[args.index("--target") + 1], self.repo.kit)
        self.assertEqual(args[args.index("--repo") + 1], REPO)
        assets = [pathlib.Path(a).name for a in args[3:6]]
        self.assertEqual(assets, [self.zip.name, "update-manifest.json", "SHA256SUMS"])
        self.assertIn(f"https://github.com/{REPO}/releases/tag/kit-0.3.6-kit.1", out)

    def test_staged_manifest_is_a_valid_private_manifest_for_the_zip(self):
        seen = {}

        class Capture(FakeGitHub):
            def __call__(inner, *args):
                if args[:2] == ("release", "create"):
                    for key, path in zip(("zip", "manifest", "sums", "notes"), (args[3], args[4], args[5], args[args.index("--notes-file") + 1])):
                        seen[key] = pathlib.Path(path).read_bytes()
                return super().__call__(*args)

        fake = Capture(self.repo.kit, self.repo.base)
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 0, err)
        manifest = json.loads(seen["manifest"])
        update_manifest.validate(manifest, expected_tag="kit-0.3.6-kit.1", expected_channel="private")
        self.assertEqual((manifest["tag"], manifest["version"], manifest["channel"]), ("kit-0.3.6-kit.1", "0.3.6-kit.1", "private"))
        self.assertEqual(manifest["build_id"], self.repo.kit)
        zip_hash = hashlib.sha256(seen["zip"]).hexdigest()
        self.assertEqual(manifest["archive"], {"name": self.zip.name, "size": len(seen["zip"]), "sha256": zip_hash})
        self.assertEqual({f["path"] for f in manifest["files"]}, set(FILES))
        sums = seen["sums"].decode().splitlines()
        self.assertEqual(sums, [f"{zip_hash}  {self.zip.name}", f"{hashlib.sha256(seen['manifest']).hexdigest()}  update-manifest.json"])
        self.assertIn("Private test kit 0.3.6-kit.1", seen["notes"].decode())

    def test_release_body_and_manifest_notes_carry_the_changelog(self):
        notes = self.root / "changes.md"
        notes.write_text("Line one\n\nLine two\n", encoding="utf-8")
        seen = {}

        class Capture(FakeGitHub):
            def __call__(inner, *args):
                if args[:2] == ("release", "create"):
                    seen["body"] = pathlib.Path(args[args.index("--notes-file") + 1]).read_text(encoding="utf-8")
                    seen["manifest"] = json.loads(pathlib.Path(args[4]).read_text(encoding="utf-8"))
                return super().__call__(*args)

        code, _, err = self.run_cli(Capture(self.repo.kit, self.repo.base), "--kit", str(self.zip), "--notes", str(notes))
        self.assertEqual(code, 0, err)
        self.assertEqual(seen["body"].strip(), "Line one\n\nLine two")
        self.assertEqual(seen["manifest"]["notes"], "Line one\n\nLine two")

    def test_kit_folder_beside_its_zip_is_accepted(self):
        folder = self.out / TOP
        folder.mkdir()
        code, out, err = self.run_cli(self.fake(), "--kit", str(folder), "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertIn("kit-0.3.6-kit.1", out)

    # --- the tag ------------------------------------------------------------------------------------------------

    def test_next_free_number_counts_releases_drafts_and_bare_tags_of_that_version_only(self):
        fake = self.fake(releases=["kit-0.3.6-kit.1", "kit-0.3.6-kit.3", "kit-0.3.5-kit.9", "v0.3.6", "kit-0.3.6-kit.x"],
                         tags=["kit-0.3.6-kit.4", "kit-0.3.60-kit.7"])
        code, out, err = self.run_cli(fake, "--kit", str(self.zip), "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertIn("tag      kit-0.3.6-kit.5 ", out)

    def test_first_kit_of_a_version_is_number_one(self):
        code, out, _ = self.run_cli(self.fake(releases=["kit-0.3.5-kit.2"]), "--kit", str(self.zip), "--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("kit-0.3.6-kit.1", out)

    def test_existing_tag_is_refused(self):
        fake = self.fake(tags=["kit-0.3.6-kit.2"])
        code, _, err = self.run_cli(fake, "--kit", str(self.zip), "--number", "2")
        self.assertEqual(code, 3)
        self.assertIn("already exists", err)
        self.assertEqual(fake.writes, [])

    def test_existing_release_tag_is_refused_even_when_listing_missed_it(self):
        fake = self.fake()
        fake.tags.add("kit-0.3.6-kit.1")  # appears only to the ref lookup, not the listing
        original = publish_kit.existing_tags
        with patch.object(publish_kit, "existing_tags", lambda repo: original(repo) - {"kit-0.3.6-kit.1"}):
            code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("already exists", err)
        self.assertEqual(fake.writes, [])

    # --- refusals -----------------------------------------------------------------------------------------------

    def test_public_repository_is_refused(self):
        fake = self.fake(private=False)
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("private repository", err)
        self.assertEqual(fake.writes, [])

    def test_zip_hash_mismatch_is_refused_before_github_is_touched(self):
        with open(self.zip, "ab") as stream:
            stream.write(b"tampered")
        fake = self.fake()
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("does not match", err)
        self.assertEqual(fake.calls, [])

    def test_missing_hash_file_or_sidecar_is_refused(self):
        (self.out / (TOP + ".zip.sha256")).unlink()
        code, _, err = self.run_cli(self.fake(), "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn(".zip.sha256 is missing", err)
        self.zip = self.make_kit()
        (self.out / (TOP + kit_manifest.SIDECAR_SUFFIX)).unlink()
        code, _, err = self.run_cli(self.fake(), "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("manifest.json is missing", err)

    def test_sidecar_that_describes_another_zip_is_refused(self):
        sidecar = self.out / (TOP + kit_manifest.SIDECAR_SUFFIX)
        manifest = json.loads(sidecar.read_text(encoding="utf-8"))
        manifest["finish"]["delivered"]["sha256"] = "c" * 64  # edit without re-hashing: finish_sha256 no longer matches
        sidecar.write_text(json.dumps(manifest), encoding="utf-8")
        fake = self.fake()
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("does not describe this zip", err)
        self.assertEqual(fake.calls, [])

    def test_release_mode_sidecar_is_refused(self):
        self.zip = self.make_kit(mode="release")
        code, _, err = self.run_cli(self.fake(), "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertTrue("only owner test kits" in err or "does not describe" in err, err)

    def test_unknown_release_version_needs_the_override(self):
        self.zip = self.make_kit(release_version=None)
        code, _, err = self.run_cli(self.fake(), "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertIn("--release-version", err)
        code, out, _ = self.run_cli(self.fake(), "--kit", str(self.zip), "--release-version", "0.3.7", "--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("kit-0.3.7-kit.1", out)

    def test_unpushed_kit_commit_targets_its_base_or_refuses(self):
        fake = self.fake(commits={self.repo.base})
        code, out, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 0, err)
        self.assertEqual(list(fake.writes[0])[list(fake.writes[0]).index("--target") + 1], self.repo.base)
        self.assertIn("kit commits are local", out)
        fake = self.fake(commits=set())
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3)
        self.assertEqual(fake.writes, [])

    def test_zip_with_two_top_folders_is_refused(self):
        with zipfile.ZipFile(self.zip, "a") as archive:  # a second top-level folder; rewrite the recorded hashes to match
            archive.writestr("other/file.txt", b"x")
        digest = hashlib.sha256(self.zip.read_bytes()).hexdigest()
        sidecar = self.out / (TOP + kit_manifest.SIDECAR_SUFFIX)
        manifest = json.loads(sidecar.read_text(encoding="utf-8"))
        for section, hash_key in (("host", "host_sha256"), ("finish", "finish_sha256")):
            body = manifest[section]
            if section == "host":
                body["outputs"][0].update(sha256=digest, size=self.zip.stat().st_size)
            else:
                body["delivered"].update(sha256=digest, size=self.zip.stat().st_size)
            manifest[hash_key] = kit_manifest.digest(body)
        manifest["finish"]["host_sha256"] = manifest["host_sha256"]
        manifest["finish_sha256"] = kit_manifest.digest(manifest["finish"])
        kit_manifest.write(sidecar, manifest)
        (self.out / (TOP + ".zip.sha256")).write_text(f"{digest}  {self.zip.name}\n", encoding="utf-8")
        fake = self.fake()
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 3, err)
        self.assertIn("top-level folder", err)
        self.assertEqual(fake.writes, [])

    def test_bad_arguments(self):
        self.assertEqual(self.run_cli(self.fake(), "--kit", str(self.zip), "--number", "0")[0], 2)
        self.assertEqual(self.run_cli(self.fake(), "--kit", str(self.zip), "--target", "abc")[0], 2)
        self.assertEqual(self.run_cli(self.fake(), "--kit", str(self.zip), "--notes", str(self.root / "none.md"))[0], 2)
        code, _, err = self.run_cli(self.fake(), "--kit", str(self.root / "missing.zip"))
        self.assertEqual(code, 3)

    # --- dry run and failures ------------------------------------------------------------------------------------

    def test_dry_run_prints_the_exact_command_and_creates_nothing(self):
        fake = self.fake()
        work = self.root / "work"
        code, out, err = self.run_cli(fake, "--kit", str(self.zip), "--dry-run", "--work", str(work))
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(fake.writes, [])
        self.assertTrue(all(call[0] in ("api", "repo") for call in fake.calls), fake.calls)
        self.assertIn("DRY RUN: nothing was created", out)
        command = next(line for line in out.splitlines() if line.startswith("command  gh "))
        for expected in ("release create kit-0.3.6-kit.1", self.zip.name, str(work / "update-manifest.json"), str(work / "SHA256SUMS"),
                         f"--repo {REPO}", f"--target {self.repo.kit}", "--latest=false"):
            self.assertIn(expected, command)
        self.assertTrue((work / "update-manifest.json").is_file())
        self.assertTrue((work / "SHA256SUMS").is_file())

    def test_dry_run_and_real_run_issue_the_same_create_command(self):
        work = self.root / "work"
        _, out, _ = self.run_cli(self.fake(), "--kit", str(self.zip), "--dry-run", "--work", str(work))
        fake = self.fake()
        self.run_cli(fake, "--kit", str(self.zip), "--work", str(work))
        printed = next(line for line in out.splitlines() if line.startswith("command  gh ")).removeprefix("command  gh ")
        self.assertEqual(printed, " ".join(f'"{a}"' if " " in a else a for a in fake.writes[0]))

    def test_gh_failure_on_create_is_reported_and_not_retried(self):
        fake = self.fake(fail_create=True)
        code, _, err = self.run_cli(fake, "--kit", str(self.zip))
        self.assertEqual(code, 4)
        self.assertIn("boom", err)
        self.assertEqual(len(fake.writes), 1)

    def test_a_release_that_ends_up_a_draft_or_prerelease_is_flagged(self):
        class Bad(FakeGitHub):
            def __call__(inner, *args):
                if args[0] == "api" and any("/releases/tags/" in a for a in args):
                    return json.dumps({"draft": True, "prerelease": True, "assets": []})
                return super().__call__(*args)

        code, out, err = self.run_cli(Bad(self.repo.kit, self.repo.base), "--kit", str(self.zip))
        self.assertEqual(code, 5)
        self.assertIn("draft", err)
        self.assertIn("pre-release", err)
        self.assertIn("releases/tag/kit-0.3.6-kit.1", out)

    def test_repo_name_is_an_argument_or_environment_never_source(self):
        fake = self.fake()
        stdout = io.StringIO()
        with patch.object(publish_kit, "gh", fake), patch.dict("os.environ", {"EAWR_KIT_REPO": "env-o/env-r"}), \
                contextlib.redirect_stdout(stdout):
            self.assertEqual(publish_kit.main(["--kit", str(self.zip), "--dry-run"]), 0)
        self.assertIn(("repo", "view", "env-o/env-r", "--json", "isPrivate,visibility,nameWithOwner"), fake.calls)
        source = (ROOT / "tools" / "release" / "publish_kit.py").read_text(encoding="utf-8")
        self.assertNotIn("Thom-Ernst", source)


class PrivateChannelManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.zip = pathlib.Path(self.temp.name) / "kit.zip"
        with zipfile.ZipFile(self.zip, "w") as archive:
            archive.writestr("top/EAWR.exe", b"x")
            archive.writestr("top/SHA256SUMS", b"y")

    def build(self, version="0.3.6-kit.2", channel="private"):
        return update_manifest.build(self.zip, version, "a" * 40, channel=channel)

    def test_private_manifest_has_the_kit_tag_and_channel(self):
        manifest = self.build()
        self.assertEqual((manifest["tag"], manifest["version"], manifest["channel"]), ("kit-0.3.6-kit.2", "0.3.6-kit.2", "private"))
        update_manifest.validate(manifest, expected_tag="kit-0.3.6-kit.2", expected_channel="private")

    def test_kit_versions_the_launcher_cannot_parse_are_refused(self):
        for bad in ("v0.3.6-kit.1", "0.3.6m-f8bf25ed", "0.3.6-kit-1", "0.3.6-", "kit-0.3.6"):
            with self.assertRaises(ValueError, msg=bad):
                self.build(bad)

    def test_channel_and_tag_must_agree_both_ways(self):
        manifest = self.build()
        manifest["channel"] = "stable"
        with self.assertRaisesRegex(ValueError, "channel and tag do not agree"):
            update_manifest.validate(manifest)
        public = update_manifest.build(self.zip, "v0.3.6", "a" * 40)
        public["channel"] = "private"
        with self.assertRaisesRegex(ValueError, "channel and tag do not agree"):
            update_manifest.validate(public)

    def test_public_builds_and_the_packager_do_not_accept_the_private_channel(self):
        self.assertEqual(update_manifest.build(self.zip, "v0.3.6", "a" * 40)["channel"], "stable")
        self.assertEqual(update_manifest.PUBLIC_CHANNELS, ("stable", "preview"))
        with self.assertRaises(ValueError):
            update_manifest.build(self.zip, "v0.3.6", "a" * 40, channel="nonsense")


if __name__ == "__main__":
    unittest.main()
