"""Real Windows exception handoff, minidump, log tail, and privacy contracts."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def read_crash(root, probe, build="0.1.0+crash-probe"):
    dumps = list(root.glob("crash-*.dmp"))
    assert len(dumps) == 1, dumps
    assert len(dumps[0].name) <= 255
    data = dumps[0].read_bytes()
    assert data[:4] == b"MDMP"
    streams, directory = struct.unpack_from("<II", data, 8)
    kinds = {struct.unpack_from("<I", data, directory + index * 12)[0] for index in range(streams)}
    assert {4, 6}.issubset(kinds), kinds
    report = (root / "crash-report.txt").read_text(encoding="utf-8")
    assert "Build: " + build in report
    assert "Exception: 0xe0424157" in report
    assert "Dump error (0 = saved): 0x0" in report
    assert probe.name.lower() + "+0x" in report.lower(), report
    return report


def main():
    probe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        home = root / "Private Player"
        home.mkdir()
        log = root / "godot.log"
        secret = str(home)
        partial = secret[:-3]
        log.write_text("discarded-prefix\n" * 2000 + "last marker\n"
                       + secret + "\\save\n" + secret.lower().replace("\\", "/")
                       + "/save\n" + secret.replace("\\", "\\\\") + "\\\\save\n"
                       + "partial marker " + partial, encoding="utf-8")
        environment = dict(os.environ, USERPROFILE=secret)
        long_path = subprocess.run([str(probe), str(log), "long-path"], env=environment, timeout=30)
        assert long_path.returncode == 0, long_path.returncode
        healthy = subprocess.run([str(probe), str(log), "healthy"], env=environment, timeout=30)
        assert healthy.returncode == 0, healthy.returncode
        assert not list(root.glob("*.dmp"))
        crashed = subprocess.run([str(probe), str(log), "crash"], env=environment, timeout=40)
        assert crashed.returncode != 0
        report = read_crash(root, probe)
        assert "last marker" in report
        assert "discarded-prefix" in report  # bounded tail still contains earlier lines
        assert "<home>" in report
        assert "private player" not in report.lower(), report
        assert partial.lower() not in report.lower(), report
        assert "partial marker <home>" in report, report
        assert "EAWR crash files (local only):" in log.read_text(encoding="utf-8")

        default = root / "default" / "logs"
        default.mkdir(parents=True)
        default_log = default / "godot.log"
        default_log.write_text("default marker\n", encoding="utf-8")
        no_arguments = subprocess.run([str(probe)],
            env=dict(environment, EAWR_CRASH_PROBE_DEFAULT_LOG=str(default_log)), timeout=40)
        assert no_arguments.returncode != 0
        assert "default marker" in read_crash(default, probe)
        assert "EAWR crash files (local only):" in default_log.read_text(encoding="utf-8")

        # Create/read through the extended namespace in Python, but give the
        # handler an ordinary absolute path. This proves its own normalization
        # without relying on either process's long-path manifest or OS policy.
        ordinary = root / ("a" * 100) / ("b" * 100) / ("c" * 100)
        assert len(str(ordinary)) > 300
        extended = Path("\\\\?\\" + str(ordinary))
        try:
            extended.mkdir(parents=True)
            (extended / "godot.log").write_text("long directory marker\n", encoding="utf-8")
            long_directory = subprocess.run([str(probe), str(ordinary / "godot.log"), "crash"],
                                            env=environment, timeout=40)
            assert long_directory.returncode != 0
            assert "long directory marker" in read_crash(extended, probe)
            assert "EAWR crash files (local only):" in (extended / "godot.log").read_text(encoding="utf-8")
        finally:
            # Keep cleanup independent of that OS policy as well.
            first = Path("\\\\?\\" + str(root / ("a" * 100)))
            if first.exists():
                shutil.rmtree(first)

        maximum = root / "maximum-build"
        # A short directory can still contain a >MAX_PATH dump when its build
        # component is maximal. Python's reader and cleanup need the namespace
        # prefix too; the handler still receives an ordinary native log path.
        extended_maximum = Path("\\\\?\\" + str(maximum))
        extended_maximum.mkdir()
        try:
            maximum_build = subprocess.run([str(probe), str(maximum / "godot.log"), "maximum-build"],
                                           env=environment, timeout=40)
            assert maximum_build.returncode != 0
            read_crash(extended_maximum, probe, "b" * 255)
        finally:
            shutil.rmtree(extended_maximum)
    print("Crash dump, exception/module streams, log tail, redaction, and teardown accepted")


if __name__ == "__main__":
    main()
