"""Retail storage and the configured host lease can use different lab roots."""
import json
import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PWSH = shutil.which("pwsh") or shutil.which("powershell.exe")


@unittest.skipUnless(PWSH, "PowerShell is required")
class RetailQueueTests(unittest.TestCase):
    def test_start_refuses_multiple_candidates_before_choosing_the_first_host(self):
        adapter = ROOT / "tools/validation/p1_capture/Invoke-EawOriginalSession.ps1"
        source = adapter.read_text()
        selection = source[source.index("    $candidates = @(if ($DebugBuild)"):source.index("    $Machine = $candidates[0].Name") + len("    $Machine = $candidates[0].Name")]
        for count in (1, 2):
            script = ("$ErrorActionPreference='Stop'; $DebugBuild=$false; $Machine='any';"
                      " function Get-RigHostTable {}; function Select-RigRetailHost { param($Table,$Machine)"
                      f" 1..{count} | ForEach-Object {{ [pscustomobject]@{{Name=('host'+$_)}} }} }};"
                      " try { " + selection + "; $Machine } catch { Write-Output $_.Exception.Message; exit 7 }")
            result = subprocess.run([PWSH, "-NoProfile", "-Command", script], capture_output=True, text=True, timeout=30)
            if count == 1:
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "host1")
            else:
                self.assertEqual(result.returncode, 7, result.stderr)
                self.assertIn("Invoke-RigQueue -Action Enter -Kind original", result.stdout)

    def run_lane(self, retail_root, fail=False):
        adapter = ROOT / "tools/validation/p1_capture/Invoke-EawOriginalSession.ps1"
        script = f"""
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$ast=[Management.Automation.Language.Parser]::ParseFile('{adapter}', [ref]$null, [ref]$null)
foreach ($name in @('New-SessionLane', 'Close-SessionLane')) {{
    $function=$ast.Find({{param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}}, $true)
    Invoke-Expression $function.Extent.Text
}}
$Machine='fixture'; $repo='.'; $script:opened=0; $script:removed=@()
function Get-RigHostTable {{ 'fixture-table' }}
function Resolve-RigHost {{ param($Table,$Name) [pscustomobject]@{{LabRoot='C:\\host-lab'}} }}
function Get-RigLabRoot {{ param($Session) $Session.LabRoot }}
function New-RigHostSession {{ param($RigHost) $script:opened++; [pscustomobject]@{{LabRoot=$RigHost.LabRoot;Id='queue'}} }}
function Remove-PSSession {{ param($Session) $script:removed+=@($Session.Id) }}
function Initialize-RigHostQueue {{
    param($Sessions,$Repo,$Table)
    if (${str(fail).lower()}) {{ throw 'fixture-init-failed' }}
    [pscustomobject]@{{Sessions=$Sessions;Root=(Get-RigLabRoot $Sessions['fixture'])}}
}}
$retail=[pscustomobject]@{{LabRoot='{retail_root}';Id='retail'}}
$lane=$null; $failure=$null
try {{ $lane=New-SessionLane $retail }} catch {{ $failure=$_.Exception.Message }}
if ($lane) {{ Close-SessionLane $lane $retail }}
@{{root=$(if($lane){{$lane.Root}}else{{$null}});retailRoot=$retail.LabRoot;opened=$script:opened;
removed=$script:removed;failure=$failure}}|ConvertTo-Json -Compress
"""
        result = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-Command", script],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_retail_storage_keeps_its_root_and_queue_uses_host_root(self):
        result = self.run_lane("D:\\retail-lab")
        self.assertEqual(result, dict(root="C:\\host-lab", retailRoot="D:\\retail-lab",
                                      opened=1, removed=["queue"], failure=None))

    def test_matching_root_reuses_the_existing_connection(self):
        result = self.run_lane("C:\\host-lab")
        self.assertEqual(result, dict(root="C:\\host-lab", retailRoot="C:\\host-lab",
                                      opened=0, removed=[], failure=None))

    def test_failed_initialization_closes_only_the_added_queue_connection(self):
        result = self.run_lane("D:\\retail-lab", fail=True)
        self.assertEqual(result, dict(root=None, retailRoot="D:\\retail-lab", opened=1,
                                      removed=["queue"], failure="fixture-init-failed"))


if __name__ == "__main__":
    unittest.main()
