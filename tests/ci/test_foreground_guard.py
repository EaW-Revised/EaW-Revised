import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPERS = ROOT / 'tools/validation/p1_capture'
sys.path.insert(0, str(HELPERS))
import foreground_guard as guard

ADAPTER = HELPERS / 'Invoke-EawOriginalSession.ps1'
SHELL = shutil.which('pwsh') or shutil.which('powershell.exe')


class ForegroundGuardTests(unittest.TestCase):
    def test_elevated_foreground_names_the_process_that_blocks_medium_input(self):
        message = guard.blocker({'runner_integrity': 8192, 'foreground_integrity': 12288,
                                 'process': 'ExampleElevated.exe', 'pid': 7})
        self.assertIn('elevated window has the foreground', message)
        self.assertIn('ExampleElevated.exe (PID 7)', message)

    def test_equal_lower_or_absent_foreground_allows_input(self):
        for level in (4096, 8192):
            self.assertIsNone(guard.blocker({'runner_integrity': 8192, 'foreground_integrity': level}))
        self.assertIsNone(guard.blocker({'status': 'no-foreground'}))

    def test_console_free_command_writes_a_result_and_preserves_inspection_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp, 'result.json')
            with mock.patch.object(guard, 'observe', return_value={
                    'status': 'observed', 'runner_integrity': 8192, 'foreground_integrity': 12288,
                    'process': 'ExampleElevated.exe', 'pid': 7}):
                self.assertEqual(guard.main(['--out', str(path)]), 0)
                self.assertIn('ExampleElevated.exe', json.loads(path.read_text())['blocker'])
            with mock.patch.object(guard, 'observe', side_effect=OSError('access denied')):
                self.assertEqual(guard.main(['--out', str(path)]), 0)
                result = json.loads(path.read_text())
                self.assertEqual(result, {'status': 'error', 'error': 'access denied'})


@unittest.skipUnless(SHELL, 'requires PowerShell')
class AdapterFocusGuardTests(unittest.TestCase):
    def test_both_connection_routes_stop_focus_before_preparing_or_sending_input(self):
        driver = r'''
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$ast = [Management.Automation.Language.Parser]::ParseFile($env:GUARD_ADAPTER, [ref]$null, [ref]$null)
foreach ($name in 'Assert-OriginalForegroundInput','Invoke-AirtestStep') {
    $node = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}, $true)
    # Functions extracted from an AST lose their file's automatic script directory.
    . ([scriptblock]::Create($node.Extent.Text.Replace('$PSScriptRoot', '$env:GUARD_HELPERS')))
}
$Machine = 'fixture'
$TaskPrefix = 'Fixture-'
$RunnerRoot = $PSScriptRoot
$desktopBlock = { 'desktop-probe' }
$script:direct = $false
$script:resolved = 0
$script:removed = 0
$script:record = [pscustomobject]@{status='observed'; blocker='elevated'; process='ExampleElevated.exe'; pid=7}
function Get-RigContext { [pscustomobject]@{Config=[pscustomobject]@{labRoot=$PSScriptRoot}; DesktopCode=''; Direct=$script:direct} }
function Read-SessionReceipt { [pscustomobject]@{run_id='owned'} }
function New-EawTestConnection { param($Name) 'connection' }
function Get-RigDirectTaskUser { param($Session,$Sid) $script:resolved++; 'resolved-user' }
function Copy-Item { param($ToSession,$LiteralPath,$Destination) }
function Remove-PSSession { param($Session) $script:removed++ }
function Invoke-Command {
    param($Session,$ScriptBlock,[object[]]$ArgumentList)
    if ($ArgumentList.Count -eq 1) { return [pscustomobject]@{one_shell=$true; active=$true; unlocked=$true; user_sid='sid'} }
    $expected = $(if ($script:direct) {'resolved-user'} else {'sid'})
    if ($ArgumentList[4] -ne $expected) { throw 'wrong interactive principal' }
    $script:record
}
foreach ($script:direct in $false,$true) {
    try { Invoke-AirtestStep 'focus' @() $PSScriptRoot; throw 'focus was not blocked' }
    catch {
        if ($_.Exception.Message -ne 'an elevated window has the foreground on fixture: ExampleElevated.exe (PID 7); dismiss it before focusing the game') { throw }
    }
}
$script:record = [pscustomobject]@{status='error'; error='access denied'}
try { Invoke-AirtestStep 'focus' @() $PSScriptRoot; throw 'inspection error was not blocked' }
catch { if ($_.Exception.Message -ne 'Cannot inspect foreground on fixture: access denied') { throw } }
if ($script:resolved -ne 2 -or $script:removed -ne 3) { throw 'connection route or cleanup was missed' }
'blocked-before-input'
'''
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp, 'driver.ps1')
            script.write_text(driver, encoding='utf-8')
            run = subprocess.run([SHELL, '-NoProfile', '-NonInteractive', '-File', str(script)],
                                 capture_output=True, text=True, timeout=30,
                                 env=dict(os.environ, GUARD_ADAPTER=str(ADAPTER), GUARD_HELPERS=str(ADAPTER.parent)))
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout.strip(), 'blocked-before-input')


if __name__ == '__main__':
    unittest.main()
