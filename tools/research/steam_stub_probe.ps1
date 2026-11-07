<#
Measures what SteamCMD (Steam's content system) does to a Workshop item folder whose files were
replaced by zero-byte stubs, deleted, edited, or joined by an extra file. Read-only towards
everything outside -Root. Run as the build account on a non-owner test machine.

Prerequisite, done once by the owner only: in -Root\steamcmd, run
    steamcmd.exe +force_install_dir <Root>\inst +login <account>
and enter the password and Steam Guard code. SteamCMD caches the login in -Root\steamcmd\config.
Nobody else enters credentials.

Usage:  powershell -NoProfile -File steam_stub_probe.ps1 -Root <scratch dir> -Account <account>
        [-Item 3802396816]
Output: <Root>\probe-<timestamp>\report.json and report.txt
#>
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Account,
    [string]$Item = '3802396816'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$cmd = Join-Path $Root 'steamcmd\steamcmd.exe'
$inst = Join-Path $Root 'inst'
$itemDir = Join-Path $inst "steamapps\workshop\content\32470\$Item"
$acf = Join-Path $inst 'steamapps\workshop\appworkshop_32470.acf'
$out = Join-Path $Root ("probe-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null

function Invoke-Download([string]$label, [switch]$Validate) {
    $a = @('+force_install_dir', $inst, '+login', $Account, '+workshop_download_item', '32470', $Item)
    if ($Validate) { $a += 'validate' }
    $a += '+quit'
    $log = & $cmd @a 2>&1
    $log | Set-Content -Encoding utf8 (Join-Path $out "$label.steamcmd.log")
    if (Test-Path $acf) { Copy-Item $acf (Join-Path $out "$label.acf") }
}

function Get-State {
    $s = @{}
    if (Test-Path $itemDir) {
        Get-ChildItem -LiteralPath $itemDir -Recurse -File -Force | ForEach-Object {
            $rel = $_.FullName.Substring($itemDir.Length + 1)
            $s[$rel] = [pscustomobject]@{ size = $_.Length; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash }
        }
    }
    return $s
}

# 1. Baseline download.
Invoke-Download 'baseline'
$base = Get-State
if ($base.Count -lt 6) { throw "item has only $($base.Count) files; pick an item with at least 6" }
$files = @($base.Keys | Sort-Object)

# 2. Mutations: half stubbed (zero bytes), two deleted, one edited, one extra file added.
$plan = [ordered]@{}
for ($i = 0; $i -lt $files.Count; $i++) {
    $f = $files[$i]; $p = Join-Path $itemDir $f
    if ($i -eq 0 -or $i -eq 1) { Remove-Item -LiteralPath $p; $plan[$f] = 'deleted' }
    elseif ($i -eq 2) { Add-Content -LiteralPath $p -Value ' ' -NoNewline; $plan[$f] = 'edited' }
    elseif ($i % 2 -eq 1) { [System.IO.File]::WriteAllBytes($p, [byte[]]@()); $plan[$f] = 'stubbed' }
    else { $plan[$f] = 'untouched' }
}
Set-Content -LiteralPath (Join-Path $itemDir 'eawr-probe-extra.txt') -Value 'extra'
$plan['eawr-probe-extra.txt'] = 'added'
$mutated = Get-State

# 3. Plain re-download (what a launch-time "is it up to date" check does).
Invoke-Download 'plain'
$plain = Get-State
# 4. Validate (what "Verify integrity" does).
Invoke-Download 'validate' -Validate
$valid = Get-State

function Describe($state, $f) {
    if (-not $state.ContainsKey($f)) { return 'missing' }
    $e = $state[$f]
    if ($base.ContainsKey($f) -and $e.sha256 -eq $base[$f].sha256) { return 'original' }
    if ($e.size -eq 0) { return 'stub' }
    return 'other-bytes'
}
$rows = foreach ($f in $plan.Keys) {
    [pscustomobject]@{
        file = $f; mutation = $plan[$f]
        after_mutation = Describe $mutated $f
        after_plain_download = Describe $plain $f
        after_validate = Describe $valid $f
    }
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $out 'report.json')
$rows | Format-Table -AutoSize | Out-String -Width 200 | Set-Content -Encoding utf8 (Join-Path $out 'report.txt')
Get-Content (Join-Path $out 'report.txt')
