"""tools/release/sign_release.py on synthetic archives: classification, dry run, sign/finish/verify, credential handling.

No network, no jsign, no Azure: the signer is replaced by a function that appends a fake certificate table to a PE
file, which is all the tool's own checks look at.
"""
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import sign_release  # noqa: E402
import update_manifest  # noqa: E402

TOP = "eaw-revised-v0.3.9-windows-x64-player"
ENV = {"SIGNING_ENDPOINT": "https://weu.codesigning.azure.net", "SIGNING_ACCOUNT": "acct",
       "SIGNING_PROFILE": "prof"}
SUBJECT = "CN=Example Maintainer"


def make_pe(payload: bytes = b"code", overlay: bytes = b"", signed: bool = False) -> bytes:
    """A minimal PE32+ file: one 0x200-byte section, optional overlay, optionally already carrying a fake signature."""
    head = bytearray(0x200)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, 0x80)
    head[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", head, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 0x98
    struct.pack_into("<H", head, opt, 0x20B)
    struct.pack_into("<I", head, opt + 60, 0x200)
    struct.pack_into("<I", head, opt + 108, 16)
    table = opt + 240
    head[table:table + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", head, table + 8, 0x200, 0x1000, 0x200, 0x200)
    section = payload.ljust(0x200, b"\0")
    data = bytes(head) + section + overlay
    return fake_sign(data) if signed else data


def fake_sign(data: bytes, blob: bytes = b"FAKE-PKCS7") -> bytes:
    """Pad to 8 bytes, append a WIN_CERTIFICATE and point the certificate directory at it (what a signer does)."""
    out = bytearray(data)
    while len(out) % 8:
        out.append(0)
    offset = len(out)
    entry = struct.pack("<IHH", 8 + len(blob), 0x200, 2) + blob
    entry += b"\0" * (-len(entry) % 8)
    out += entry
    pe = sign_release.parse_pe(bytes(out[:65536]))
    struct.pack_into("<II", out, pe.cert_dir, offset, len(entry))
    struct.pack_into("<I", out, pe.checksum, 0xABCD)  # checksum changes too
    return bytes(out)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_zip(path: Path, top: str, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(f"{top}/{name}", (2026, 11, 18, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def make_release(root: Path, extra: dict[str, bytes] | None = None, version: str = "v0.3.9") -> Path:
    release = root / "release"
    release.mkdir()
    launcher, mods, extension = make_pe(b"launcher", overlay=b"xyz"), make_pe(b"mods"), make_pe(b"ext")
    godot = make_pe(b"godot")
    files = {"EAWR.exe": launcher, "EAWR Mods.exe": mods, "godot/Godot_v4.7.2-stable_win64.exe": godot,
             "project/bin/libeawr_viewer.windows.template_release.x86_64.dll": extension,
             "README.md": b"hello\n", **(extra or {})}
    runtime = {"schema": "eawr-player-runtime/1", "godot_sha256": sha(godot), "launcher_sha256": sha(launcher),
               "extension_sha256": sha(extension)}
    files["runtime.json"] = (json.dumps(runtime, indent=2) + "\n").encode()
    files["SHA256SUMS"] = "".join(f"{sha(d)}  {n}\n" for n, d in sorted(files.items())).encode()
    player = release / f"eaw-revised-{version}-windows-x64-player.zip"
    write_zip(player, TOP, files)
    tools = release / f"eaw-revised-{version}-windows-x64-tools.zip"
    write_zip(tools, "eaw-revised-v0.3.9-windows-x64-tools", {"bin/sim_headless.exe": make_pe(b"sim"), "README.md": b"t\n"})
    update_manifest.write(player, release, version, "abc123def", None, "")
    names = [player.name, tools.name, update_manifest.MANIFEST_NAME]
    (release / "SHA256SUMS.txt").write_text("".join(f"{sha((release / n).read_bytes())}  {n}\n" for n in sorted(names)),
                                           encoding="utf-8")
    return release


def fake_signer(path: Path) -> None:
    path.write_bytes(fake_sign(path.read_bytes()))


class SignReleaseTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def run_chain(self, release: Path, signer=fake_signer):
        signed, final = self.root / "signed", self.root / "final"
        self.assertEqual(sign_release.sign(release, signed, ENV, signer=signer), 0)
        self.assertEqual(sign_release.finish(release, signed, final), 0)
        return signed, final

    def test_comparable_digest_ignores_signature_padding_and_checksum(self):
        for overlay in (b"", b"abc", b"12345678"):
            plain = make_pe(overlay=overlay)
            signed = fake_sign(plain)
            self.assertNotEqual(plain, signed)
            self.assertEqual(sign_release.comparable_digest(plain), sign_release.comparable_digest(signed))
        self.assertNotEqual(sign_release.comparable_digest(make_pe(b"a")), sign_release.comparable_digest(make_pe(b"b")))
        self.assertNotEqual(sign_release.comparable_digest(make_pe(overlay=b"a")),
                            sign_release.comparable_digest(make_pe(overlay=b"b")))

    def test_plan_classifies_and_refuses_unknown_binaries(self):
        release = make_release(self.root)
        result = sign_release.plan(release, sign_release.load_targets())
        kinds = {(m.archive, m.rel): m.kind for ms in result.values() for m in ms}
        self.assertEqual(kinds[(f"eaw-revised-v0.3.9-windows-x64-player.zip", "EAWR.exe")], "sign")
        self.assertEqual(kinds[("eaw-revised-v0.3.9-windows-x64-player.zip", "godot/Godot_v4.7.2-stable_win64.exe")], "never")
        self.assertEqual(kinds[("eaw-revised-v0.3.9-windows-x64-tools.zip", "bin/sim_headless.exe")], "sign")
        other = self.root / "other"
        other.mkdir()
        unknown = make_release(other, {"extra/new_tool.exe": make_pe(b"new")}, version="v0.3.8")
        with self.assertRaisesRegex(sign_release.SigningError, "new_tool.exe"):
            sign_release.plan(unknown, sign_release.load_targets())
        third = self.root / "third"
        third.mkdir()
        foreign = make_release(third, {"extra/vendor.dll": make_pe(b"vendor", signed=True)}, version="v0.3.7")
        kinds = {m.rel: m.kind for ms in sign_release.plan(foreign, sign_release.load_targets()).values() for m in ms}
        self.assertEqual(kinds["extra/vendor.dll"], "foreign")

    def test_plan_checks_hashes_tag_and_request(self):
        release = make_release(self.root)
        targets = sign_release.load_targets()
        sums = sha((release / "SHA256SUMS.txt").read_bytes())
        sign_release.plan(release, targets, "v0.3.9", sums)
        with self.assertRaisesRegex(sign_release.SigningError, "request named"):
            sign_release.plan(release, targets, "v0.3.9", "0" * 64)
        with self.assertRaisesRegex(sign_release.SigningError, "request named v0.4.0"):
            sign_release.plan(release, targets, "v0.4.0")
        (release / "eaw-revised-v0.3.9-windows-x64-tools.zip").write_bytes(b"tampered")
        with self.assertRaisesRegex(sign_release.SigningError, "does not match"):
            sign_release.plan(release, targets)

    def test_dry_run_without_variables_signs_nothing(self):
        release = make_release(self.root)
        out = self.root / "signed"
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(sign_release.sign(release, out, {}), 0)
        self.assertIn("DRY RUN", buffer.getvalue())
        self.assertFalse(out.exists())
        partial = dict(ENV)
        del partial["SIGNING_ACCOUNT"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sign_release.sign(release, out, partial), 0)
        self.assertFalse(out.exists())

    def test_sign_finish_verify_roundtrip(self):
        release = make_release(self.root)
        with contextlib.redirect_stdout(io.StringIO()):
            signed, final = self.run_chain(release)
        receipt = json.loads((final / "signing-receipt.json").read_text())
        self.assertEqual(sorted(e["path"] for e in receipt["files"]),
                         ["EAWR Mods.exe", "EAWR.exe", "bin/sim_headless.exe",
                          "project/bin/libeawr_viewer.windows.template_release.x86_64.dll"])
        player = final / "eaw-revised-v0.3.9-windows-x64-player.zip"
        with zipfile.ZipFile(player) as new, zipfile.ZipFile(release / player.name) as old:
            self.assertEqual(new.namelist(), old.namelist())
            engine = f"{TOP}/godot/Godot_v4.7.2-stable_win64.exe"
            self.assertEqual(new.read(engine), old.read(engine))
            runtime = json.loads(new.read(f"{TOP}/runtime.json"))
            launcher = new.read(f"{TOP}/EAWR.exe")
            self.assertEqual(runtime["launcher_sha256"], sha(launcher))
            self.assertEqual(runtime["godot_sha256"], json.loads(old.read(f"{TOP}/runtime.json"))["godot_sha256"])
            sums = sign_release.read_sums_text(new.read(f"{TOP}/SHA256SUMS").decode())
            self.assertEqual(sums["EAWR.exe"], sha(launcher))
            self.assertEqual(sums["README.md"], sha(new.read(f"{TOP}/README.md")))
        manifest = json.loads((final / "update-manifest.json").read_text())
        self.assertEqual(manifest["archive"]["sha256"], sha(player.read_bytes()))
        self.assertEqual(manifest["build_id"], "abc123def")
        self.assertEqual(manifest["signature"], "")
        self.assertEqual(sign_release.read_sums(final / "SHA256SUMS.txt")[player.name], sha(player.read_bytes()))
        self.assertEqual(sign_release.verify(release, final, subject=SUBJECT), [])

    def test_verify_reports_unsigned_changed_and_inconsistent_files(self):
        release = make_release(self.root)
        with contextlib.redirect_stdout(io.StringIO()):
            _, final = self.run_chain(release)
        player = final / "eaw-revised-v0.3.9-windows-x64-player.zip"
        # an allowlisted file put back unsigned
        broken = self.root / "broken"
        broken.mkdir()
        for item in final.iterdir():
            (broken / item.name).write_bytes(item.read_bytes())
        with zipfile.ZipFile(release / player.name) as old:
            sign_release.rewrite_zip(broken / player.name, broken / player.name,
                                     {f"{TOP}/EAWR Mods.exe": old.read(f"{TOP}/EAWR Mods.exe")})
        problems = sign_release.verify(release, broken, subject=SUBJECT)
        self.assertTrue(any("EAWR Mods.exe is not signed" in p for p in problems), problems)
        # a data file changed
        sign_release.rewrite_zip(player, broken / player.name, {f"{TOP}/README.md": b"changed\n"})
        problems = sign_release.verify(release, broken, subject=SUBJECT)
        self.assertTrue(any("README.md changed" in p for p in problems), problems)

    def test_signer_must_only_add_a_signature(self):
        release = make_release(self.root)

        def nothing(path: Path) -> None:
            pass

        def damaging(path: Path) -> None:
            path.write_bytes(fake_sign(path.read_bytes().replace(b"launcher", b"LAUNCHER")))

        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(sign_release.SigningError, "without a signature"):
                sign_release.sign(release, self.root / "a", ENV, signer=nothing)
            with self.assertRaisesRegex(sign_release.SigningError, "more than the signature"):
                sign_release.sign(release, self.root / "b", ENV, signer=damaging)

    def test_already_signed_target_is_refused(self):
        release = make_release(self.root, {"EAWR Mods.exe": make_pe(b"x", signed=True)})
        with self.assertRaisesRegex(sign_release.SigningError, "already signed"):
            sign_release.plan(release, sign_release.load_targets())

    def test_finish_needs_a_receipt(self):
        release = make_release(self.root)
        (self.root / "empty").mkdir()
        with self.assertRaisesRegex(sign_release.SigningError, "nothing was signed"):
            sign_release.finish(release, self.root / "empty", self.root / "final")

    def test_account_values_and_endpoint_check(self):
        self.assertIsNone(sign_release.account_values({}))
        self.assertEqual(sign_release.account_values(ENV), {"host": "weu.codesigning.azure.net", "alias": "acct/prof"})
        self.assertEqual(sign_release.account_values({**ENV, "SIGNING_ENDPOINT": "weu.codesigning.azure.net"})["host"],
                         "weu.codesigning.azure.net")
        with self.assertRaises(sign_release.SigningError):
            sign_release.account_values({**ENV, "SIGNING_ENDPOINT": "https://example.com"})
        with self.assertRaises(sign_release.SigningError):
            sign_release.account_values({**ENV, "SIGNING_ACCOUNT": "a/b"})

    def test_configured_signing_without_a_credential_route_fails(self):
        release = make_release(self.root)
        jar = self.root / "jsign.jar"
        jar.write_bytes(b"not the pinned jar")
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(sign_release.SigningError, "pinned jsign"):
                sign_release.sign(release, self.root / "o", ENV, jar=jar)
        tools = sign_release.load_json(sign_release.TOOLS)
        with self.assertRaisesRegex(sign_release.SigningError, "no credential route"):
            sign_release.access_token({}, tools)
        self.assertEqual(sign_release.access_token({"SIGNING_ACCESS_TOKEN": "t"}, tools), "t")

    def test_jsign_gets_the_token_only_through_the_environment(self):
        tools = sign_release.load_json(sign_release.TOOLS)
        account = {"host": "weu.codesigning.azure.net", "alias": "acct/prof"}
        seen = {}

        def fake_run(command, env=None, **kwargs):
            seen["command"], seen["env"] = command, env
            return mock.Mock(returncode=1, stdout="denied with TOKEN123", stderr="")

        with mock.patch.object(sign_release.subprocess, "run", fake_run):
            with self.assertRaises(sign_release.SigningError) as raised:
                sign_release.run_jsign(Path("x.exe"), account, "TOKEN123", tools, Path("jsign.jar"), {"PATH": "p"})
        self.assertNotIn("TOKEN123", " ".join(seen["command"]))
        self.assertNotIn("TOKEN123", str(raised.exception))
        self.assertEqual(seen["env"]["SIGNING_ACCESS_TOKEN"], "TOKEN123")
        for flag in ("TRUSTEDSIGNING", "weu.codesigning.azure.net", "env:SIGNING_ACCESS_TOKEN", "acct/prof",
                     "http://timestamp.acs.microsoft.com", "RFC3161"):
            self.assertIn(flag, seen["command"])

    def test_repository_configuration_is_consistent(self):
        targets = sign_release.load_targets()
        self.assertEqual(targets.classify("godot/Godot_v4.7.2-stable_win64.exe", False), "never")
        self.assertEqual(targets.classify("EAWR.exe", False), "sign")
        self.assertEqual(targets.classify("eawr.EXE", False), "sign")
        self.assertEqual(targets.classify("nested/EAWR.exe", False), "unknown")
        tools = sign_release.load_json(sign_release.TOOLS)
        pem = (ROOT / tools["root_ca"]["file"]).read_text()
        der = ssl.PEM_cert_to_DER_cert(pem)
        self.assertEqual(sha(der), tools["root_ca"]["sha256_fingerprint"])
        self.assertRegex(tools["jsign"]["sha256"], r"^[0-9a-f]{64}$")
        self.assertTrue(tools["timestamp"]["url"].startswith("http://timestamp.acs.microsoft.com"))

    def test_command_line_exit_codes(self):
        release = make_release(self.root)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sign_release.main(["plan", str(release)], {}), 0)
            self.assertEqual(sign_release.main(["sign", str(release), "--out", str(self.root / "s")], {}), 0)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(sign_release.main(["plan", str(self.root / "missing")], {}), 2)
            self.assertEqual(sign_release.main(["finish", str(release), "--signed", str(self.root / "s"),
                                                "--out", str(self.root / "f")], {}), 2)

    def test_sign_test_signs_only_the_throwaway_binary(self):
        good = self.root / "eawr-signing-test.exe"
        good.write_bytes(make_pe(b"throwaway"))
        out = self.root / "out"
        with contextlib.redirect_stdout(io.StringIO()) as buffer:
            self.assertEqual(sign_release.sign_test(good, out, {}), 0)
        self.assertIn("DRY RUN", buffer.getvalue())
        self.assertFalse(out.exists())
        wrong = self.root / "EAWR.exe"
        wrong.write_bytes(make_pe(b"x"))
        with self.assertRaisesRegex(sign_release.SigningError, "must be named"):
            sign_release.sign_test(wrong, out, ENV, signer=fake_signer)
        signed = self.root / "signed-in.exe"
        signed.write_bytes(b"")
        pre_signed = self.root / "pre" / "eawr-signing-test.exe"
        pre_signed.parent.mkdir()
        pre_signed.write_bytes(make_pe(b"x", signed=True))
        with self.assertRaisesRegex(sign_release.SigningError, "unsigned PE"):
            sign_release.sign_test(pre_signed, out, ENV, signer=fake_signer)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sign_release.sign_test(good, out, ENV, signer=fake_signer, osslsigncode="ossl",
                                                    runner=lambda exe, path, ca: (True, "Subject: x")), 0)
        receipt = json.loads((out / "signing-test-receipt.json").read_text())
        self.assertEqual(receipt["file"], "eawr-signing-test.exe")
        self.assertTrue(receipt["osslsigncode_ok"])
        self.assertNotEqual(receipt["unsigned_sha256"], receipt["signed_sha256"])
        self.assertTrue(sign_release.parse_pe((out / "eawr-signing-test.exe").read_bytes()[:65536]).signed)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(sign_release.SigningError, "osslsigncode did not accept"):
                sign_release.sign_test(good, out, ENV, signer=fake_signer, osslsigncode="ossl",
                                       runner=lambda exe, path, ca: (False, "bad"))

    def test_comparable_digest_covers_the_gaps_between_headers_and_sections(self):
        plain = bytearray(make_pe(b"code"))
        struct.pack_into("<I", plain, 0x98 + 60, 0x100)   # SizeOfHeaders ends 0x100 bytes before the first section
        tampered = bytearray(plain)
        tampered[0x1F0] ^= 0xFF                            # a byte in the gap
        digest = sign_release.comparable_digest
        self.assertNotEqual(digest(bytes(plain)), digest(bytes(tampered)))
        self.assertEqual(digest(bytes(plain)), digest(fake_sign(bytes(plain))))
        self.assertNotEqual(digest(fake_sign(bytes(plain))), digest(fake_sign(bytes(tampered))))
        # data after the certificate table counts as well
        signed = fake_sign(make_pe())
        self.assertNotEqual(digest(signed), digest(signed + b"x"))
        # a certificate table that does not fit the file is refused
        broken = bytearray(signed)
        pe = sign_release.parse_pe(bytes(broken[:65536]))
        struct.pack_into("<I", broken, pe.cert_dir + 4, len(broken))
        with self.assertRaisesRegex(sign_release.SigningError, "certificate table"):
            digest(bytes(broken))

    def test_verify_requires_the_expected_signer_subject(self):
        release = make_release(self.root)
        with contextlib.redirect_stdout(io.StringIO()):
            _, final = self.run_chain(release)
        for empty in ("", "  "):
            problems = sign_release.verify(release, final, subject=empty)
            self.assertTrue(any("no expected signer subject" in p for p in problems), problems)
        problems = sign_release.verify(release, final, "ossl", True, runner=lambda exe, path, ca: (True, "Subject: anything"))
        self.assertTrue(any("no expected signer subject" in p for p in problems), problems)
        wrong = sign_release.verify(release, final, "ossl", True, SUBJECT,
                                    runner=lambda exe, path, ca: (True, "Subject: CN=Someone Else"))
        self.assertTrue(any("not signed by the expected subject" in p for p in wrong), wrong)
        right = sign_release.verify(release, final, "ossl", True, SUBJECT,
                                    runner=lambda exe, path, ca: (True, f"Subject: {SUBJECT}"))
        self.assertEqual(right, [])
        # the command line: no --subject and no SIGNING_SUBJECT fails, either source passes
        with mock.patch.object(sign_release.shutil, "which", return_value=None):
            with contextlib.redirect_stderr(io.StringIO()) as err, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(sign_release.main(["verify", str(release), "--final", str(final)], {}), 1)
                self.assertEqual(sign_release.main(["verify", str(release), "--final", str(final), "--subject", SUBJECT], {}), 0)
                self.assertEqual(sign_release.main(["verify", str(release), "--final", str(final)],
                                                   {"SIGNING_SUBJECT": SUBJECT}), 0)
        self.assertIn("no expected signer subject", err.getvalue())

    def test_verify_signatures_script_requires_the_player_files(self):
        script = (ROOT / "tools/release/verify_signatures.ps1").read_text(encoding="utf-8")
        suffixes = re.search(r"\$peSuffixes = ([^\r\n]+)", script)
        self.assertIsNotNone(suffixes)
        self.assertEqual(tuple(re.findall(r"'(\.[a-z]+)'", suffixes.group(1))), sign_release.PE_SUFFIXES)
        self.assertIn("required_in_player", script)
        targets = sign_release.load_targets()
        required = sign_release.load_json(sign_release.TARGETS)["required_in_player"]
        self.assertTrue(required)
        for entry in required:
            self.assertEqual(targets.classify(entry, False), "sign", entry)
        pwsh = shutil.which("pwsh")
        if sys.platform == "win32" and pwsh:
            folder = self.root / "player"
            folder.mkdir()
            (folder / "README.md").write_text("x", encoding="utf-8")
            done = subprocess.run([pwsh, "-NoProfile", "-File", str(ROOT / "tools/release/verify_signatures.ps1"),
                                   "-Folder", str(folder)], capture_output=True, text=True, timeout=120)
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            for name in ("EAWR.exe", "eawr_loadcheck.exe", "required file is missing"):
                self.assertIn(name, done.stdout)

    def test_job_guard_allows_only_sign_release_from_main(self):
        guard = ROOT / "tools/release/signing_host/job-guard.sh"
        text = guard.read_text(encoding="utf-8")
        for value in ("EaW-Revised/EaW-Revised", "EaW-Revised/EaW-Revised/.github/workflows/sign-release.yml@refs/heads/main",
                      "workflow_dispatch", "refs/heads/main"):
            self.assertIn(f'"{value}"', text)
        setup = (ROOT / "tools/release/signing_host/setup-signing-host.sh").read_text(encoding="utf-8")
        self.assertIn('install -m 0755 -o root -g root "$HERE/job-guard.sh" "$TOOLS_DIR/job-guard.sh"', setup)
        self.assertIn("ACTIONS_RUNNER_HOOK_JOB_STARTED=$TOOLS_DIR/job-guard.sh", setup)
        self.assertIn("job guard hook:", setup)     # selfcheck reports it
        self.assertIn("job guard: installed", setup)
        readme = (ROOT / "tools/release/signing_host/README.md").read_text(encoding="utf-8")
        self.assertIn("The job guard", readme)
        bash = shutil.which("bash")
        if sys.platform == "win32":
            bash = next((c for c in (r"C:\Program Files\Git\bin\bash.exe", r"C:\Program Files (x86)\Git\bin\bash.exe")
                         if Path(c).is_file()), None)
        if not bash:
            self.skipTest("no POSIX bash to run the guard")
        allowed = {"GITHUB_REPOSITORY": "EaW-Revised/EaW-Revised", "GITHUB_EVENT_NAME": "workflow_dispatch",
                   "GITHUB_REF": "refs/heads/main",
                   "GITHUB_WORKFLOW_REF": "EaW-Revised/EaW-Revised/.github/workflows/sign-release.yml@refs/heads/main"}
        base = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}

        def run(overrides):
            env = {**base, **{k: v for k, v in {**allowed, **overrides}.items() if v is not None}}
            return subprocess.run([bash, guard.as_posix()], env=env, capture_output=True, text=True, timeout=60)

        self.assertEqual(run({}).returncode, 0)
        bad = {"GITHUB_REPOSITORY": "someone/else", "GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/feature",
               "GITHUB_WORKFLOW_REF": "EaW-Revised/EaW-Revised/.github/workflows/sign-release.yml@refs/heads/feature"}
        for name, value in bad.items():
            for override in ({name: value}, {name: None}):
                done = run(override)
                self.assertEqual(done.returncode, 1, (name, override))
                self.assertIn(name, done.stderr)
                self.assertEqual(len(done.stderr.strip().splitlines()), 1)
                self.assertNotIn(value, done.stderr)

    def test_there_is_only_one_certificate_profile(self):
        for path in ("tools/release/sign_release.py", "tools/release/signing_host/README.md", "docs/releasing.md",
                     "tools/oss/public/.github/workflows/sign-release.yml", "tools/release/verify_signatures.ps1"):
            text = (ROOT / path).read_text(encoding="utf-8")
            self.assertNotIn("TEST_PROFILE", text, path)
            self.assertNotRegex(text, r"(?i)public trust test|profile-kind|profile=test", path)

    def test_workflow_is_gated_and_pinned(self):
        text = (ROOT / "tools/oss/public/.github/workflows/sign-release.yml").read_text(encoding="utf-8")
        triggers = text.split("permissions:", 1)[0]
        self.assertIn("workflow_dispatch:", triggers)
        for forbidden in ("pull_request", "push:", "schedule:", "workflow_run", "release:"):
            self.assertNotIn(forbidden, triggers)
        self.assertIn("runs-on: [self-hosted, linux, eawr-signing]", text)
        self.assertNotIn("ubuntu-", text)
        self.assertIn("environment: release-signing", text)
        # main only, manual only: a job-level condition plus a first step that fails loudly
        gate = "if: ${{ github.ref == 'refs/heads/main' && github.event_name == 'workflow_dispatch' }}"
        self.assertIn(gate, text)
        self.assertLess(text.index(gate), text.index("runs-on:"))
        first = text.split("    steps:\n", 1)[1].split("- name:")[1]
        self.assertIn("Refuse to run anywhere but main", first)
        for needle in ('"$GITHUB_REF" = "refs/heads/main"', '"$GITHUB_EVENT_NAME" = "workflow_dispatch"',
                       "sign-release.yml@refs/heads/main", "exit 1"):
            self.assertIn(needle, first)
        self.assertLess(text.index("Refuse to run anywhere but main"), text.index("actions/checkout"))
        # the expected signer is always passed to the release verify
        self.assertIn('--subject "$SIGNING_SUBJECT"', text)
        self.assertIn("id-token: write", text)
        self.assertNotIn("secrets.", text)
        for line in text.splitlines():
            if line.strip().startswith("uses:"):
                self.assertRegex(line, r"@[0-9a-f]{40}")
        for step in ("plan", "sign", "finish", "verify"):
            self.assertIn(f"sign_release.py {step} ", text)
        # the request inputs reach the shell only through env, never by expression inside a script
        for block in text.split("run: |")[1:]:
            script = block.split("- name:")[0]
            self.assertNotIn("${{", script)

    def test_no_secret_shaped_text_in_the_signing_files(self):
        for path in (ROOT / "tools/release/sign_release.py", ROOT / "config/signing-tools.json",
                     ROOT / "config/signing-targets.json"):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("eyJ", text)
            self.assertNotRegex(text, r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


if __name__ == "__main__":
    unittest.main()
