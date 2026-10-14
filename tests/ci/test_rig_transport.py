"""Native validation keeps bundle hashes and plain-path guards per request."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PWSH = shutil.which("pwsh") or shutil.which("powershell.exe")

# The overlay's guarded source seam, independent of private sibling tooling.
FIXTURE = r"""
param($root,$expected)
function Assert-ContainedPlainPath($path,$root) {
    $full=[IO.Path]::GetFullPath($path)
    $base=[IO.Path]::GetFullPath($root).TrimEnd('\')
    if (-not $full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'outside' }
    $part=$full
    while($part.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $part) {
            if ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'reparse' }
        }
        if($part -eq $base){break}; $part=Split-Path $part -Parent
    }
    $full
}
$file=Assert-ContainedPlainPath (Join-Path $root 'bundle.bin') $root
if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) { throw 'hash mismatch' }
$part=$file
# Second attributes seam belongs to artifact checks, which remain active.
if ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'reparse' }
'passed'
"""

@unittest.skipUnless(PWSH and os.name == "nt", "Windows PowerShell transport required")
class TransportTests(unittest.TestCase):
    def test_optimized_validation_rechecks_hash_and_rejects_source_drift(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp)
            source=root/'transport.ps1'
            source.write_text(FIXTURE)
            script=f"""
$ErrorActionPreference='Stop'
. '{ROOT / 'tools/rig/RigRetail.ps1'}'
$root='{root}'
$file=Join-Path $root 'bundle.bin'
[IO.File]::WriteAllText($file,'original')
$hash=(Get-FileHash $file).Hash
Optimize-RigAirtestTransport '{source}'
$good=& '{source}' $root $hash
[IO.File]::WriteAllText($file,'changed')
$bad=$null; try {{ & '{source}' $root $hash }} catch {{ $bad=$_.Exception.Message }}
$drift=$null; try {{ Optimize-RigAirtestTransport '{source}' }} catch {{ $drift=$_.Exception.Message }}
@{{good=$good;bad=$bad;drift=$drift}} | ConvertTo-Json -Compress
"""
            run=subprocess.run([PWSH,'-NoProfile','-ExecutionPolicy','Bypass','-Command',script],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            result=json.loads(run.stdout)
            self.assertEqual(result['good'],'passed')
            self.assertEqual(result['bad'],'hash mismatch')
            self.assertIn('refusing an incomplete optimization',result['drift'])

    def test_optimized_validation_rejects_reparse_ancestor(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp)
            source=root/'transport.ps1'; source.write_text(FIXTURE)
            script=f"""
$ErrorActionPreference='Stop'
. '{ROOT / 'tools/rig/RigRetail.ps1'}'
$root='{root}'
$target=New-Item -ItemType Directory (Join-Path $root 'target')
[IO.File]::WriteAllText((Join-Path $target.FullName 'bundle.bin'),'original')
$link=Join-Path $root 'linked'
New-Item -ItemType Junction -Path $link -Target $target.FullName | Out-Null
try {{
 Optimize-RigAirtestTransport '{source}'
 $hash=(Get-FileHash (Join-Path $target.FullName 'bundle.bin')).Hash
 $bad=$null; try {{ & '{source}' $link $hash }} catch {{ $bad=$_.Exception.Message }}
 @{{bad=$bad}}|ConvertTo-Json -Compress
}} finally {{ [IO.Directory]::Delete($link) }}
"""
            run=subprocess.run([PWSH,'-NoProfile','-ExecutionPolicy','Bypass','-Command',script],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertEqual(json.loads(run.stdout)['bad'],'reparse')

if __name__ == '__main__':
    unittest.main()
