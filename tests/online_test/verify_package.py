"""Builds the online test kit zip with the packaging script and checks what a tester gets."""
import json
import pathlib
from pe_imports import verify_pe_imports
import socket
import subprocess
import sys
import zipfile


def main():
    cmake, exe, package_dir, script, out_dir, dlls = sys.argv[1:]
    out = pathlib.Path(out_dir)
    done = subprocess.run([cmake, f"-DPython3_EXECUTABLE={sys.executable}", f"-DEXE={exe}", f"-DDLLS={dlls}", f"-DPACKAGE_DIR={package_dir}", f"-DOUT_DIR={out}",
                           "-DADDRESS=online-test.example.invalid", "-DPORT=27020", "-DTRUST=system-ca", "-P", script],
                          capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stdout + done.stderr
    archive = out / "EAWR-online-test.zip"
    with zipfile.ZipFile(archive) as bundle:
        names = {pathlib.PurePosixPath(n).name for n in bundle.namelist() if not n.endswith("/")}
        expected = {pathlib.Path(exe).name, "EAWR-online-test-host.cmd", "EAWR-online-test-join.cmd", "server.conf"}
        expected |= {pathlib.Path(d).name for d in dlls.split(";") if d}
        assert names == expected, (sorted(names), sorted(expected))
        assert all(n.startswith("EAWR-online-test/") for n in bundle.namelist()), bundle.namelist()
        for role in ("host", "join"):
            data = bundle.read(f"EAWR-online-test/EAWR-online-test-{role}.cmd")
            text = data.decode("ascii")
            assert b"\r\n" in data and b"\n" not in data.replace(b"\r\n", b""), "launcher must use CRLF"
            assert f"eawr_online_test.exe {role}" in text and "pause" in text and "set /p" not in text.lower(), text
        if sys.platform == "win32":
            files = {pathlib.PurePosixPath(n).name: bundle.read(n) for n in bundle.namelist() if not n.endswith("/")}
            dependents = verify_pe_imports(files)
            (out / "pe-imports.json").write_text(json.dumps(dependents, indent=2) + "\n")
            print("PE dependents:", json.dumps(dependents, sort_keys=True))
        else:
            print("PE import check skipped (not win32)")
        conf = bundle.read("EAWR-online-test/server.conf").decode()
        assert "address=online-test.example.invalid" in conf and "port=27020" in conf and "trust=system-ca" in conf, conf
    validation = pathlib.Path(script).with_name("EawrOnlineTestConfig.cmake")
    for address, trust in (("play.example.org", "system-ca"), ("127.0.0.1", "system-ca"),
                           ("online-test.example.invalid", "pin:abc"), ("bad/host", "system-ca"),
                           ("-bad.example", "system-ca"), ("a" * 254, "system-ca"),
                           ("999.1.2.3", "pin:" + "a" * 64), ("a" * 64 + ".org", "system-ca"),
                           ("bad-.example", "system-ca"), ("a..b", "system-ca")):
        invalid = out / "invalid-config.cmake"
        invalid.write_text(f'include("{validation.as_posix()}")\neawr_validate_online_test_config("{address}" "27020" "{trust}")\n')
        rejected = subprocess.run([cmake, f"-DPython3_EXECUTABLE={sys.executable}", "-P", str(invalid)], capture_output=True, text=True, timeout=30)
        assert rejected.returncode != 0, (address, trust)
    # Smoke the packaged exe, then parse the actual shipped server.conf through its real parser.
    extract = out / "unzipped"
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(extract)
    kit = extract / "EAWR-online-test"
    (kit / pathlib.Path(exe).name).chmod(0o755)  # zip extraction drops the mode bit on POSIX; Windows testers do not need it
    with socket.socket() as closed:
        closed.bind(("127.0.0.1", 0))
        port = str(closed.getsockname()[1])
    ran = subprocess.run([str(kit / pathlib.Path(exe).name), "host", "--address", "127.0.0.1", "--port", port, "--pin", "a" * 64, "--timeout", "30"],
                         capture_output=True, text=True, timeout=90, cwd=str(kit))
    if ran.returncode == 3:
        return 77
    assert ran.returncode == 1 and "RESULT: FAIL" in ran.stdout, (ran.returncode, ran.stdout, ran.stderr)
    parsed = subprocess.run([str(kit / pathlib.Path(exe).name), "host", "--config", str(kit / "server.conf"),
                             "--port", port, "--timeout", "30"],
                            capture_output=True, text=True, timeout=90, cwd=str(kit))
    assert parsed.returncode == 1 and "RESULT: FAIL" in parsed.stdout, (parsed.returncode, parsed.stdout, parsed.stderr)
    assert not any("ONLINE-TEST-" + kind in parsed.stdout + parsed.stderr for kind in ("ARGUMENT", "CONFIG", "ADDRESS", "TRUST")), parsed.stdout + parsed.stderr
    print("packaged server.conf parsed by the real native parser and reached the network stage")
    log = (kit / "online-test-host.log").read_text()
    assert "online-test.example.invalid" not in log and "127.0.0.1" not in log, log
    print("online test kit zip:", archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
