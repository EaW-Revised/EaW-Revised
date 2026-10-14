"""Windows PowerShell native stderr must not turn successful provisioning into failure."""
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell.exe")


@unittest.skipUnless(POWERSHELL, "Windows PowerShell is required for native stderr semantics")
class NativeProvisioningTests(unittest.TestCase):
    def run_provision(self, failed_phase=""):
        source = (ROOT / "tools/rig/RigCommon.ps1").read_text()
        marker = "$previousActionPreference = $ErrorActionPreference"
        block = marker + source.split(marker, 1)[1].split(
            '[IO.File]::WriteAllText("$work\\env\\env.json"', 1
        )[0]
        with tempfile.TemporaryDirectory() as directory:
            fake_python = pathlib.Path(directory) / "python-fixture.cmd"
            fake_python.write_text(
                '@echo off\n'
                'echo WARNING: fixture script directory is not on PATH. 1>&2\n'
                'if "%1"=="-c" goto verify\n'
                'if "%2"=="%EAWR_TEST_FAIL_PHASE%" exit /b 7\n'
                'goto success\n'
                ':verify\n'
                'if "%EAWR_TEST_FAIL_PHASE%"=="verify" exit /b 7\n'
                ':success\n'
                'echo fixture-package-versions\n'
                'exit /b 0\n'
            )
            quoted = str(fake_python).replace("'", "''")
            script = pathlib.Path(directory) / "test.ps1"
            script.write_text(
                "$ErrorActionPreference = 'Stop'\n"
                f"$py = '{quoted}'\n"
                f"$work = '{directory}'\n"
                f"$env:EAWR_TEST_FAIL_PHASE = '{failed_phase}'\n"
                "$failure = $null\n"
                "try {\n" + block + "\n} catch { $failure = $_.Exception.Message }\n"
                "@{ failure=$failure; preference=[string]$ErrorActionPreference; "
                "versions=[string]($versions -join ' ') } | ConvertTo-Json -Compress\n"
            )
            result = subprocess.run(
                [POWERSHELL, "-NoProfile", "-NonInteractive", "-Command", script.read_text()],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return json.loads(result.stdout.strip())

    def test_successful_commands_can_warn_on_stderr(self):
        result = self.run_provision()
        self.assertIsNone(result["failure"])
        self.assertIn("fixture-package-versions", result["versions"])
        self.assertEqual(result["preference"], "Stop")

    def test_nonzero_exit_still_fails_and_restores_preference(self):
        for phase, message in (
            ("ensurepip", "ensurepip failed"),
            ("pip", "pip install failed"),
            ("verify", "Python package verification failed"),
        ):
            with self.subTest(phase=phase):
                result = self.run_provision(phase)
                self.assertIn(message, result["failure"])
                self.assertEqual(result["preference"], "Stop")


if __name__ == "__main__":
    unittest.main()
