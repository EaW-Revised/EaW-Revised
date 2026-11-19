#!/usr/bin/env python3
"""Authenticode signing of the Windows release archives (docs/releasing.md, "Code signing").

The pool packages and hashes the release UNSIGNED, as always. This tool runs afterwards, on the dedicated signing
host (tools/release/signing_host/) or as a dry run anywhere, and never on a build worker:

  plan    <release-dir>                       classify every Windows executable in the archives; check the hashes
  sign    <release-dir> --out <signed-dir>    sign the allowlisted executables with Azure Artifact Signing (jsign)
  sign-test <eawr-signing-test.exe> --out <dir>  sign one throwaway binary with the same profile, to prove the route
  finish  <release-dir> --signed <dir> --out <final-dir>
                                              put the signed files back, rewrite runtime.json, the folder SHA256SUMS,
                                              update-manifest.json and SHA256SUMS.txt (no credentials needed)
  verify  <release-dir> --final <final-dir>   prove the final set: same code bytes, every allowlisted file signed,
                                              hashes and manifest consistent (osslsigncode when available)

A release directory holds the archives (*.zip), SHA256SUMS.txt and update-manifest.json as the packager wrote them.

No credential is ever read from a file or the repository. `sign` takes the Azure account values from environment
variables (set as GitHub environment variables, never committed) and gets a short-lived access token from the GitHub
OIDC token of the running job (or from SIGNING_ACCESS_TOKEN for an attended run). It passes the token to jsign through
the environment only. Without the account variables `sign` is a dry run: it classifies, signs nothing and exits 0.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Callable

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "common"))
import hidden_console  # noqa: E402
hidden_console.install()
sys.path.insert(0, str(HERE))
import update_manifest  # noqa: E402

TARGETS = ROOT / "config" / "signing-targets.json"
TOOLS = ROOT / "config" / "signing-tools.json"
RECEIPT = "signing-receipt.json"
SUMS = "SHA256SUMS.txt"
FOLDER_SUMS = "SHA256SUMS"
RUNTIME = "runtime.json"
PE_SUFFIXES = (".exe", ".dll", ".sys", ".ocx", ".scr", ".cpl")
DEFAULT_JAR = "/opt/eawr-signing/jsign-7.6.jar"
ENV_ENDPOINT, ENV_ACCOUNT, ENV_PROFILE = "SIGNING_ENDPOINT", "SIGNING_ACCOUNT", "SIGNING_PROFILE"
TEST_BINARY = "eawr-signing-test.exe"
ENV_TOKEN = "SIGNING_ACCESS_TOKEN"


class SigningError(RuntimeError):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Configuration

def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def _glob(pattern: str) -> re.Pattern[str]:
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out), re.IGNORECASE)


class Targets:
    def __init__(self, data: dict[str, Any]):
        if data.get("schema") != 1:
            raise SigningError("signing-targets.json: unsupported schema")
        self.sign = [_glob(p) for p in data["sign"]]
        self.never = [(_glob(e["pattern"]), e["reason"]) for e in data["never_sign"]]

    def classify(self, rel: str, signed_already: bool) -> str:
        if any(rx.fullmatch(rel) for rx in self.sign):
            return "sign"
        if any(rx.fullmatch(rel) for rx, _ in self.never):
            return "never"
        return "foreign" if signed_already else "unknown"


def load_targets(path: Path = TARGETS) -> Targets:
    return Targets(load_json(path))


# ---------------------------------------------------------------------------------------------------------------
# PE files

class Pe:
    def __init__(self, checksum: int, cert_dir: int | None, headers: int, sections: list[tuple[int, int]],
                 cert_off: int, cert_size: int):
        self.checksum, self.cert_dir, self.headers, self.sections = checksum, cert_dir, headers, sections
        self.cert_off, self.cert_size = cert_off, cert_size

    @property
    def signed(self) -> bool:
        return self.cert_size > 0


def parse_pe(head: bytes) -> Pe | None:
    """The fields Authenticode hashing needs, from the first bytes of a file; None when it is not a PE file."""
    if len(head) < 0x40 or head[:2] != b"MZ":
        return None
    e = struct.unpack_from("<I", head, 0x3C)[0]
    if e + 24 > len(head) or head[e:e + 4] != b"PE\0\0":
        return None
    sections = struct.unpack_from("<H", head, e + 6)[0]
    optional_size = struct.unpack_from("<H", head, e + 20)[0]
    opt = e + 24
    if opt + 64 > len(head):
        return None
    magic = struct.unpack_from("<H", head, opt)[0]
    if magic == 0x10B:
        dirs = opt + 96
    elif magic == 0x20B:
        dirs = opt + 112
    else:
        return None
    table = opt + optional_size
    if table + 40 * sections > len(head) or dirs + 8 * 5 > len(head):
        raise SigningError("PE headers are larger than the 64 KiB read for classification")
    count = struct.unpack_from("<I", head, dirs - 4)[0]
    cert_dir = dirs + 4 * 8 if count >= 5 else None
    cert_off, cert_size = struct.unpack_from("<II", head, cert_dir) if cert_dir is not None else (0, 0)
    headers = struct.unpack_from("<I", head, opt + 60)[0]
    spans = []
    for n in range(sections):
        raw_size, raw_ptr = struct.unpack_from("<II", head, table + 40 * n + 16)
        spans.append((raw_ptr, raw_size))
    return Pe(opt + 64, cert_dir, headers, spans, cert_off, cert_size)


def comparable_digest(data: bytes) -> str:
    """SHA-256 over every byte of a PE file except the three things a signer changes: the checksum field, the
    certificate-directory entry and the certificate table itself (plus the zero padding that precedes the table).
    Bytes between the headers and the first section, and between sections, count too. Equal for a file and its signed
    copy, so it proves that signing changed nothing but the signature."""
    pe = parse_pe(data[:65536])
    if pe is None or pe.cert_dir is None:
        raise SigningError("not a PE file with a certificate directory")
    if pe.signed:
        if pe.cert_off < pe.headers or pe.cert_off + pe.cert_size > len(data):
            raise SigningError("the certificate table lies outside the file or inside the headers")
        body_end, tail = pe.cert_off, data[pe.cert_off + pe.cert_size:]
    else:
        body_end, tail = len(data), b""
    body = data[:pe.checksum] + data[pe.checksum + 4:pe.cert_dir] + data[pe.cert_dir + 8:body_end]
    return sha256_bytes(body.rstrip(b"\0") + tail)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------------------------
# Archives

class Member:
    def __init__(self, archive: str, rel: str, size: int, kind: str):
        self.archive, self.rel, self.size, self.kind = archive, rel, size, kind


def _top(archive: zipfile.ZipFile, name: str) -> str:
    tops = {i.filename.split("/", 1)[0] for i in archive.infolist() if not i.is_dir()}
    if len(tops) != 1:
        raise SigningError(f"{name}: expected exactly one top-level folder")
    return tops.pop()


def scan_zip(path: Path, targets: Targets) -> tuple[str, list[Member]]:
    members: list[Member] = []
    with zipfile.ZipFile(path) as archive:
        top = _top(archive, path.name)
        for info in archive.infolist():
            if info.is_dir():
                continue
            rel = info.filename[len(top) + 1:]
            with archive.open(info) as stream:
                head = stream.read(65536)
            pe = parse_pe(head)
            if pe is None:
                if rel.lower().endswith(PE_SUFFIXES) and head[:2] == b"MZ":
                    raise SigningError(f"{path.name}: {rel} looks like a Windows binary but its PE header is not readable")
                continue
            members.append(Member(path.name, rel, info.file_size, targets.classify(rel, pe.signed)))
            if members[-1].kind == "sign" and pe.signed:
                raise SigningError(f"{path.name}: {rel} is already signed")
    return top, members


def release_zips(release_dir: Path) -> list[Path]:
    zips = sorted(release_dir.glob("*.zip"))
    if not zips:
        raise SigningError(f"{release_dir}: no archives")
    return zips


def read_sums(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise SigningError(f"{path.name}: malformed line {line[:60]!r}")
        out[match.group(2)] = match.group(1)
    return out


def check_release_sums(release_dir: Path) -> dict[str, str]:
    sums_file = release_dir / SUMS
    if not sums_file.is_file():
        raise SigningError(f"{release_dir}: {SUMS} is missing")
    sums = read_sums(sums_file)
    names = [z.name for z in release_zips(release_dir)]
    manifest = release_dir / update_manifest.MANIFEST_NAME
    if manifest.is_file():
        names.append(manifest.name)
    for name in names:
        if name not in sums:
            raise SigningError(f"{SUMS} does not list {name}")
        if sha256_file(release_dir / name) != sums[name]:
            raise SigningError(f"{name} does not match {SUMS}")
    return sums


def rewrite_zip(src: Path, dst: Path, replace: dict[str, bytes]) -> None:
    """Copy src to dst member by member (same order, dates and attributes), with some members replaced."""
    missing = set(replace)
    tmp = dst.with_name(dst.name + ".tmp")
    try:
        with zipfile.ZipFile(src) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                new = zipfile.ZipInfo(info.filename, info.date_time)
                new.compress_type = zipfile.ZIP_DEFLATED
                new.external_attr = info.external_attr
                new.create_system = info.create_system
                if info.filename in replace:
                    missing.discard(info.filename)
                    zout.writestr(new, replace[info.filename])
                else:
                    new.file_size = info.file_size
                    with zin.open(info) as source, zout.open(new, "w") as target:
                        shutil.copyfileobj(source, target, 1 << 20)
        if missing:
            raise SigningError(f"{src.name}: cannot replace missing members {sorted(missing)}")
        os.replace(tmp, dst)
    finally:
        tmp.unlink(missing_ok=True)


# ---------------------------------------------------------------------------------------------------------------
# plan

def plan(release_dir: Path, targets: Targets, tag: str = "", sums_sha256: str = "") -> dict[str, list[Member]]:
    if sums_sha256 and sha256_file(release_dir / SUMS) != sums_sha256.lower():
        raise SigningError(f"{SUMS} does not have the SHA-256 the signing request named")
    check_release_sums(release_dir)
    manifest_file = release_dir / update_manifest.MANIFEST_NAME
    if manifest_file.is_file():
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        update_manifest.validate(manifest)
        if tag and manifest["tag"] != tag:
            raise SigningError(f"the update manifest is for {manifest['tag']}, the request named {tag}")
    elif tag:
        raise SigningError("a tag was requested but the release directory has no update manifest")
    result: dict[str, list[Member]] = {}
    unknown = []
    for archive in release_zips(release_dir):
        _, members = scan_zip(archive, targets)
        result[archive.name] = members
        unknown += [f"{m.archive}: {m.rel}" for m in members if m.kind == "unknown"]
    if unknown:
        raise SigningError("Windows binaries with no decision in config/signing-targets.json: " + "; ".join(unknown))
    return result


def print_plan(result: dict[str, list[Member]]) -> None:
    for name, members in result.items():
        for m in members:
            print(f"{m.kind:8} {name}: {m.rel}")


# ---------------------------------------------------------------------------------------------------------------
# credentials and jsign

def account_values(environ: dict[str, str]) -> dict[str, str] | None:
    """Endpoint host, account and profile, or None when the signing variables are not all set (dry run)."""
    values = {k: environ.get(k, "").strip() for k in (ENV_ENDPOINT, ENV_ACCOUNT, ENV_PROFILE)}
    if not all(values.values()):
        return None
    host = urllib.parse.urlsplit(values[ENV_ENDPOINT] if "//" in values[ENV_ENDPOINT] else "//" + values[ENV_ENDPOINT]).hostname
    if not host or not host.endswith(".codesigning.azure.net"):
        raise SigningError(f"{ENV_ENDPOINT} is not an Artifact Signing endpoint")
    if "/" in values[ENV_ACCOUNT] or "/" in values[ENV_PROFILE]:
        raise SigningError("account and profile names must not contain '/'")
    return {"host": host, "alias": f"{values[ENV_ACCOUNT]}/{values[ENV_PROFILE]}"}


def _post_form(url: str, fields: dict[str, str]) -> dict[str, Any]:
    request = urllib.request.Request(url, urllib.parse.urlencode(fields).encode(), method="POST",
                                     headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read())


def access_token(environ: dict[str, str], tools: dict[str, Any]) -> str:
    """A short-lived Artifact Signing token: from the running GitHub job's OIDC token (federated credential, no
    secret), or from SIGNING_ACCESS_TOKEN for an attended run."""
    if environ.get(ENV_TOKEN):
        return environ[ENV_TOKEN]
    request_url, request_token = environ.get("ACTIONS_ID_TOKEN_REQUEST_URL"), environ.get("ACTIONS_ID_TOKEN_REQUEST_TOKEN")
    tenant, client = environ.get("AZURE_TENANT_ID", ""), environ.get("AZURE_CLIENT_ID", "")
    if not (request_url and request_token and tenant and client):
        raise SigningError("signing is configured but there is no credential route: run it as the release-signing job "
                           "(OIDC), or set SIGNING_ACCESS_TOKEN for an attended run")
    audience = urllib.parse.quote(tools["oidc"]["audience"], safe="")
    request = urllib.request.Request(f"{request_url}&audience={audience}", headers={"Authorization": f"Bearer {request_token}"})
    with urllib.request.urlopen(request, timeout=60) as response:
        assertion = json.loads(response.read())["value"]
    if environ.get("GITHUB_ACTIONS") == "true":
        print(f"::add-mask::{assertion}")
    answer = _post_form(f"https://login.microsoftonline.com/{urllib.parse.quote(tenant)}/oauth2/v2.0/token", {
        "client_id": client, "grant_type": "client_credentials", "scope": tools["oidc"]["scope"],
        "client_assertion_type": "urn:ietf:params:oauth:client-assertion-type:jwt-bearer", "client_assertion": assertion})
    token = answer["access_token"]
    if environ.get("GITHUB_ACTIONS") == "true":
        print(f"::add-mask::{token}")
    return token


def check_jar(jar: Path, tools: dict[str, Any]) -> None:
    if not jar.is_file():
        raise SigningError(f"jsign jar not found: {jar}")
    if sha256_file(jar) != tools["jsign"]["sha256"]:
        raise SigningError(f"{jar.name} does not match the pinned jsign {tools['jsign']['version']} (config/signing-tools.json)")


def run_jsign(path: Path, account: dict[str, str], token: str, tools: dict[str, Any], jar: Path,
              environ: dict[str, str]) -> None:
    command = [environ.get("JAVA", "java"), "-jar", str(jar), "--storetype", "TRUSTEDSIGNING", "--keystore", account["host"],
               "--storepass", f"env:{ENV_TOKEN}", "--alias", account["alias"], "--alg", "SHA-256",
               "--tsaurl", tools["timestamp"]["url"], "--tsmode", tools["timestamp"]["mode"], str(path)]
    env = dict(environ)
    env[ENV_TOKEN] = token
    done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=600)
    if done.returncode != 0:
        text = (done.stdout + done.stderr).replace(token, "***")
        raise SigningError(f"jsign failed for {path.name} (exit {done.returncode}): {text[-800:]}")


Signer = Callable[[Path], None]


def sign(release_dir: Path, out_dir: Path, environ: dict[str, str], jar: Path | None = None,
         signer: Signer | None = None, tag: str = "", sums_sha256: str = "") -> int:
    tools = load_json(TOOLS)
    result = plan(release_dir, load_targets(), tag, sums_sha256)
    print_plan(result)
    account = account_values(environ)
    if account is None:
        print("signing variables absent: DRY RUN, nothing was signed (set SIGNING_ENDPOINT, SIGNING_ACCOUNT and "
              "SIGNING_PROFILE)")
        return 0
    if signer is None:
        jar = jar or Path(environ.get("SIGNING_JSIGN_JAR", DEFAULT_JAR))
        check_jar(jar, tools)
        token = access_token(environ, tools)
        signer = lambda path: run_jsign(path, account, token, tools, jar, environ)  # noqa: E731
    out_dir.mkdir(parents=True, exist_ok=True)
    receipt: dict[str, Any] = {"schema": 1, "mode": "signed",
                               "tool": {"jsign": tools["jsign"]["version"], "jsign_sha256": tools["jsign"]["sha256"]},
                               "timestamp": tools["timestamp"]["url"], "files": []}
    for archive_name, members in result.items():
        todo = [m for m in members if m.kind == "sign"]
        if not todo:
            continue
        replace: dict[str, bytes] = {}
        src = release_dir / archive_name
        with zipfile.ZipFile(src) as archive, tempfile.TemporaryDirectory(prefix="eawr-sign-") as work:
            top = _top(archive, archive_name)
            for index, member in enumerate(todo):
                path = Path(work) / str(index) / Path(member.rel).name
                path.parent.mkdir()
                unsigned = archive.read(f"{top}/{member.rel}")
                path.write_bytes(unsigned)
                signer(path)
                signed = path.read_bytes()
                pe = parse_pe(signed[:65536])
                if pe is None or not pe.signed:
                    raise SigningError(f"{member.rel}: the signer returned a file without a signature")
                digest = comparable_digest(unsigned)
                if comparable_digest(signed) != digest:
                    raise SigningError(f"{member.rel}: signing changed more than the signature")
                replace[f"{top}/{member.rel}"] = signed
                receipt["files"].append({"archive": archive_name, "path": member.rel, "unsigned_sha256": sha256_bytes(unsigned),
                                         "signed_sha256": sha256_bytes(signed), "code_digest": digest})
        rewrite_zip(src, out_dir / archive_name, replace)
    (out_dir / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"signed {len(receipt['files'])} files; receipt {out_dir / RECEIPT}")
    return 0


# ---------------------------------------------------------------------------------------------------------------
# test signing

def sign_test(binary: Path, out_dir: Path, environ: dict[str, str], jar: Path | None = None, signer: Signer | None = None,
              osslsigncode: str = "", runner: Callable[[str, Path, Path], tuple[bool, str]] | None = None) -> int:
    """Signs one throwaway binary (exactly named eawr-signing-test.exe, never part of a release) with the real profile
    and checks the result, to prove the whole route before a release depends on it. The signature is real and timestamped."""
    if binary.name != TEST_BINARY:
        raise SigningError(f"the test binary must be named {TEST_BINARY}")
    data = binary.read_bytes()
    pe = parse_pe(data[:65536])
    if pe is None or pe.signed:
        raise SigningError(f"{binary.name} must be an unsigned PE file")
    tools = load_json(TOOLS)
    account = account_values(environ)
    if account is None:
        print("signing variables absent: DRY RUN, nothing was signed")
        return 0
    if signer is None:
        jar = jar or Path(environ.get("SIGNING_JSIGN_JAR", DEFAULT_JAR))
        check_jar(jar, tools)
        token = access_token(environ, tools)
        signer = lambda path: run_jsign(path, account, token, tools, jar, environ)  # noqa: E731
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / TEST_BINARY
    target.write_bytes(data)
    signer(target)
    signed = target.read_bytes()
    pe_signed = parse_pe(signed[:65536])
    if pe_signed is None or not pe_signed.signed:
        raise SigningError("the signer returned a file without a signature")
    if comparable_digest(signed) != comparable_digest(data):
        raise SigningError("signing changed more than the signature")
    receipt: dict[str, Any] = {"schema": 1, "mode": "test", "file": TEST_BINARY, "unsigned_sha256": sha256_bytes(data),
                               "signed_sha256": sha256_bytes(signed), "code_digest": comparable_digest(data),
                               "tool": {"jsign": tools["jsign"]["version"]}, "timestamp": tools["timestamp"]["url"]}
    if osslsigncode:
        ok, text = (runner or run_osslsigncode)(osslsigncode, target, ROOT / tools["root_ca"]["file"])
        (out_dir / "osslsigncode-verify.txt").write_text(text, encoding="utf-8", newline="\n")
        receipt["osslsigncode_ok"] = ok
        if not ok:
            write_json(out_dir / "signing-test-receipt.json", receipt)
            raise SigningError("osslsigncode did not accept the test signature (see osslsigncode-verify.txt)")
    write_json(out_dir / "signing-test-receipt.json", receipt)
    print(f"signed {TEST_BINARY}; receipt {out_dir / 'signing-test-receipt.json'}")
    return 0


# ---------------------------------------------------------------------------------------------------------------
# finish

def _replace_hashes(node: Any, mapping: dict[str, str]) -> Any:
    if isinstance(node, str):
        return mapping.get(node, node)
    if isinstance(node, list):
        return [_replace_hashes(v, mapping) for v in node]
    if isinstance(node, dict):
        return {k: _replace_hashes(v, mapping) for k, v in node.items()}
    return node


def finish(release_dir: Path, signed_dir: Path, out_dir: Path) -> int:
    receipt_file = signed_dir / RECEIPT
    if not receipt_file.is_file():
        raise SigningError(f"{signed_dir}: no {RECEIPT}; nothing was signed (dry run?)")
    receipt = load_json(receipt_file)
    if receipt.get("mode") != "signed":
        raise SigningError("the receipt is not a signing receipt")
    old_sums = check_release_sums(release_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    by_archive: dict[str, list[dict[str, Any]]] = {}
    for entry in receipt["files"]:
        by_archive.setdefault(entry["archive"], []).append(entry)
    for name in old_sums:
        if name == update_manifest.MANIFEST_NAME:
            continue
        src = signed_dir / name if (signed_dir / name).is_file() else release_dir / name
        entries = by_archive.get(name, [])
        replace: dict[str, bytes] = {}
        if entries:
            mapping = {e["unsigned_sha256"]: e["signed_sha256"] for e in entries}
            by_path = {e["path"]: e["signed_sha256"] for e in entries}
            with zipfile.ZipFile(src) as archive:
                top = _top(archive, name)
                names = set(archive.namelist())
                runtime_name, sums_name = f"{top}/{RUNTIME}", f"{top}/{FOLDER_SUMS}"
                if runtime_name in names:
                    runtime = json.loads(archive.read(runtime_name))
                    replace[runtime_name] = (json.dumps(_replace_hashes(runtime, mapping), indent=2) + "\n").encode("utf-8")
                if sums_name in names:
                    # the folder sums cover runtime.json too, and that file was just rewritten
                    if runtime_name in replace:
                        by_path[RUNTIME] = sha256_bytes(replace[runtime_name])
                    lines = []
                    for line in archive.read(sums_name).decode("utf-8").splitlines():
                        digest, path = line.split("  ", 1)
                        lines.append(f"{by_path.get(path, digest)}  {path}\n")
                    replace[sums_name] = "".join(lines).encode("utf-8")
            rewrite_zip(src, out_dir / name, replace)
        else:
            shutil.copyfile(src, out_dir / name)
    manifest_file = release_dir / update_manifest.MANIFEST_NAME
    if manifest_file.is_file():
        old = json.loads(manifest_file.read_text(encoding="utf-8"))
        update_manifest.validate(old)
        manifest = update_manifest.build(out_dir / old["archive"]["name"], old["version"], old["build_id"], old["channel"], old["notes"])
        (out_dir / update_manifest.MANIFEST_NAME).write_text(update_manifest.render(manifest), encoding="utf-8", newline="\n")
    lines = []
    for name in old_sums:
        lines.append(f"{sha256_file(out_dir / name)}  {name}\n")
    (out_dir / SUMS).write_text("".join(lines), encoding="utf-8", newline="\n")
    receipt["mode"] = "signed"
    receipt["final"] = {name: sha256_file(out_dir / name) for name in old_sums}
    (out_dir / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"final set in {out_dir}")
    return 0


# ---------------------------------------------------------------------------------------------------------------
# verify

def run_osslsigncode(exe: str, path: Path, ca_file: Path) -> tuple[bool, str]:
    done = subprocess.run([exe, "verify", "-CAfile", str(ca_file), "-in", str(path)], capture_output=True, text=True, timeout=300)
    return done.returncode == 0, done.stdout + done.stderr


def verify(release_dir: Path, final_dir: Path, osslsigncode: str = "", require_ossl: bool = False, subject: str = "",
           runner: Callable[[str, Path, Path], tuple[bool, str]] = run_osslsigncode) -> list[str]:
    """Every problem found, empty when the final set is consistent. The expected signer `subject` is required: the
    pinned Microsoft root also trusts other Artifact Signing customers, so a chain check alone proves nothing about
    who signed."""
    problems: list[str] = []
    if not subject.strip():
        problems.append("no expected signer subject: pass --subject (release mode sets SIGNING_SUBJECT); "
                        "a signature chain alone does not say whose it is")
    targets = load_targets()
    tools = load_json(TOOLS)
    old_sums = check_release_sums(release_dir)
    if not (final_dir / SUMS).is_file():
        return [f"{final_dir}: {SUMS} is missing"]
    new_sums = read_sums(final_dir / SUMS)
    if list(new_sums) != list(old_sums):
        problems.append(f"{SUMS} lists different files than the unsigned release")
    for name, digest in new_sums.items():
        if not (final_dir / name).is_file() or sha256_file(final_dir / name) != digest:
            problems.append(f"{name} does not match the final {SUMS}")
    ca_file = ROOT / tools["root_ca"]["file"]
    if osslsigncode:
        if not ca_file.is_file():
            problems.append(f"root certificate file missing: {ca_file}")
    elif require_ossl:
        problems.append("osslsigncode is required but not found")
    signed_count = 0
    for archive_path in release_zips(release_dir):
        name = archive_path.name
        final_path = final_dir / name
        if not final_path.is_file():
            problems.append(f"{name} is missing from the final set")
            continue
        with zipfile.ZipFile(archive_path) as old, zipfile.ZipFile(final_path) as new:
            if old.namelist() != new.namelist():
                problems.append(f"{name}: members differ from the unsigned archive")
                continue
            top = _top(new, name)
            folder_sums = {}
            if f"{top}/{FOLDER_SUMS}" in new.namelist():
                folder_sums = read_sums_text(new.read(f"{top}/{FOLDER_SUMS}").decode("utf-8"))
            for info in new.infolist():
                if info.is_dir():
                    continue
                rel = info.filename[len(top) + 1:]
                with new.open(info) as stream:
                    head = stream.read(65536)
                pe = parse_pe(head)
                kind = targets.classify(rel, pe.signed) if pe is not None else ""
                if kind == "sign":
                    data, original = new.read(info.filename), old.read(info.filename)
                    pe_new = parse_pe(data[:65536])
                    if not pe_new or not pe_new.signed:
                        problems.append(f"{name}: {rel} is not signed")
                        continue
                    signed_count += 1
                    if comparable_digest(data) != comparable_digest(original):
                        problems.append(f"{name}: {rel} differs from the unsigned file beyond the signature")
                    if osslsigncode:
                        with tempfile.TemporaryDirectory(prefix="eawr-verify-") as work:
                            target = Path(work) / Path(rel).name
                            target.write_bytes(data)
                            ok, text = runner(osslsigncode, target, ca_file)
                        if not ok:
                            problems.append(f"{name}: osslsigncode refused {rel}")
                        elif subject.strip() and subject not in text:
                            problems.append(f"{name}: {rel} is not signed by the expected subject")
                    new_sha = sha256_bytes(data)
                elif kind in ("never", "foreign", "unknown"):
                    if kind == "unknown":
                        problems.append(f"{name}: {rel} has no signing decision")
                    new_sha = _member_sha(new, info.filename)
                    if new_sha != _member_sha(old, info.filename):
                        problems.append(f"{name}: {rel} changed but is not a signing target")
                else:
                    new_sha = _member_sha(new, info.filename)
                    if rel not in (RUNTIME, FOLDER_SUMS) and new_sha != _member_sha(old, info.filename):
                        problems.append(f"{name}: {rel} changed")
                if folder_sums and rel != FOLDER_SUMS and folder_sums.get(rel) != new_sha:
                    problems.append(f"{name}: the folder {FOLDER_SUMS} does not match {rel}")
    manifest_file = final_dir / update_manifest.MANIFEST_NAME
    if manifest_file.is_file():
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        try:
            update_manifest.validate(manifest)
            rebuilt = update_manifest.build(final_dir / manifest["archive"]["name"], manifest["version"], manifest["build_id"],
                                            manifest["channel"], manifest["notes"])
            if update_manifest.render(rebuilt) != manifest_file.read_text(encoding="utf-8"):
                problems.append("update-manifest.json does not describe the final player archive")
        except ValueError as error:
            problems.append(str(error))
    if signed_count == 0:
        problems.append("no signed executable found in the final set")
    return problems


def _member_sha(archive: zipfile.ZipFile, name: str) -> str:
    h = hashlib.sha256()
    with archive.open(name) as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_sums_text(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match:
            out[match.group(2)] = match.group(1)
    return out


# ---------------------------------------------------------------------------------------------------------------

def main(argv: list[str] | None = None, environ: dict[str, str] | None = None) -> int:
    environ = dict(os.environ) if environ is None else dict(environ)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan")
    p.add_argument("release_dir", type=Path)
    p.add_argument("--tag", default="")
    p.add_argument("--sums-sha256", default="")
    s = sub.add_parser("sign")
    s.add_argument("release_dir", type=Path)
    s.add_argument("--out", type=Path, required=True)
    s.add_argument("--jar", type=Path)
    s.add_argument("--tag", default="")
    s.add_argument("--sums-sha256", default="")
    t = sub.add_parser("sign-test", help="sign the throwaway eawr-signing-test.exe with the real profile")
    t.add_argument("binary", type=Path)
    t.add_argument("--out", type=Path, required=True)
    t.add_argument("--jar", type=Path)
    t.add_argument("--osslsigncode", default="")
    f = sub.add_parser("finish")
    f.add_argument("release_dir", type=Path)
    f.add_argument("--signed", type=Path, required=True)
    f.add_argument("--out", type=Path, required=True)
    v = sub.add_parser("verify")
    v.add_argument("release_dir", type=Path)
    v.add_argument("--final", type=Path, required=True)
    v.add_argument("--osslsigncode", default="")
    v.add_argument("--require-osslsigncode", action="store_true")
    v.add_argument("--subject", default="", help="text the signer certificate must contain; required (default: SIGNING_SUBJECT)")
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            print_plan(plan(args.release_dir, load_targets(), args.tag, args.sums_sha256))
            return 0
        if args.command == "sign":
            return sign(args.release_dir, args.out, environ, args.jar, None, args.tag, args.sums_sha256)
        if args.command == "sign-test":
            return sign_test(args.binary, args.out, environ, args.jar, None, args.osslsigncode or shutil.which("osslsigncode") or "")
        if args.command == "finish":
            return finish(args.release_dir, args.signed, args.out)
        exe = args.osslsigncode or shutil.which("osslsigncode") or ""
        problems = verify(args.release_dir, args.final, exe, args.require_osslsigncode,
                          args.subject or environ.get("SIGNING_SUBJECT", ""))
        for problem in problems:
            print(f"verify: {problem}", file=sys.stderr)
        if not problems:
            print("verify: ok" + ("" if exe else " (osslsigncode not available: signature chain not checked here; run "
                                  "tools/release/verify_signatures.ps1 on Windows)"))
        return 1 if problems else 0
    except (SigningError, OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError, KeyError,
            urllib.error.URLError, subprocess.SubprocessError) as error:
        print(f"sign_release: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
