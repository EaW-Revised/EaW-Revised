<#
.SYNOPSIS
  Checks the Authenticode signatures of an extracted release folder on Windows (docs/releasing.md, "Code signing").

.DESCRIPTION
  Every Windows binary (.exe .dll .sys .ocx .scr .cpl) that config/signing-targets.json lists for signing must carry a
  valid signature with a trusted chain and a timestamp, and the signer subject must contain -ExpectedSubject when one is
  given. Every file named in the list's required_in_player entries must also exist in the folder: a signed binary that
  was dropped from the archive is a failure, not a silent pass. Engine files that the list says never to sign are
  reported but not required. Exit 0 when everything required passes, 1 otherwise.

  Run it on the extracted folder of the archive a player would download. It reads nothing from the network.
  -Required overrides the required_in_player list (patterns relative to the folder, same wildcards as the target list);
  pass an empty array only for a folder that is not a player archive (for example the tools archive).

.EXAMPLE
  pwsh tools/release/verify_signatures.ps1 -Folder C:\test\eaw-revised-v0.3.9-windows-x64-player -ExpectedSubject '<legal name on the certificate>'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Folder,
    [string]$Targets = (Join-Path $PSScriptRoot '..\..\config\signing-targets.json'),
    [string]$ExpectedSubject = '',
    [string[]]$Required
)
$ErrorActionPreference = 'Stop'

# the same suffixes as PE_SUFFIXES in sign_release.py
$peSuffixes = '.exe', '.dll', '.sys', '.ocx', '.scr', '.cpl'

function ConvertTo-GlobRegex([string]$Pattern) {
    $sb = [System.Text.StringBuilder]::new('^')
    for ($i = 0; $i -lt $Pattern.Length; $i++) {
        if ($Pattern.Substring($i).StartsWith('**')) { [void]$sb.Append('.*'); $i++ }
        elseif ($Pattern[$i] -eq '*') { [void]$sb.Append('[^/]*') }
        elseif ($Pattern[$i] -eq '?') { [void]$sb.Append('[^/]') }
        else { [void]$sb.Append([regex]::Escape([string]$Pattern[$i])) }
    }
    [void]$sb.Append('$')
    [regex]::new($sb.ToString(), 'IgnoreCase')
}

$config = Get-Content -LiteralPath $Targets -Raw | ConvertFrom-Json
$sign = @($config.sign | ForEach-Object { ConvertTo-GlobRegex $_ })
$never = @($config.never_sign | ForEach-Object { ConvertTo-GlobRegex $_.pattern })
if (-not $PSBoundParameters.ContainsKey('Required')) { $Required = @($config.required_in_player | Where-Object { $_ }) }
$root = (Get-Item -LiteralPath $Folder).FullName.TrimEnd('\')
$failures = 0
$files = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension.ToLowerInvariant() -in $peSuffixes })
$present = @($files | ForEach-Object { [System.IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/') })
$rows = @(foreach ($pattern in $Required) {
    $rx = ConvertTo-GlobRegex $pattern
    if (-not ($present | Where-Object { $rx.IsMatch($_) })) {
        $script:failures++
        [pscustomobject]@{ File = $pattern; Role = 'required'; Status = 'Missing'; Signer = ''; Timestamped = $false; Problem = 'required file is missing from the folder' }
    }
})
$rows += foreach ($file in $files) {
    $rel = [System.IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
    $required = [bool]($sign | Where-Object { $_.IsMatch($rel) })
    $skipped = [bool]($never | Where-Object { $_.IsMatch($rel) })
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
    $problem = ''
    if ($required) {
        $acceptable = $signature.Status -eq 'Valid'
        if (-not $acceptable) { $problem = "status $($signature.Status)" }
        elseif (-not $signature.TimeStamperCertificate) { $problem = 'no timestamp' }
        elseif ($ExpectedSubject -and -not $subject.Contains($ExpectedSubject)) { $problem = 'unexpected signer' }
    }
    elseif (-not $skipped) { $problem = 'no signing decision (not in signing-targets.json)' ; if ($signature.Status -eq 'Valid') { $problem = '' } }
    if ($problem) { $script:failures++ }
    [pscustomobject]@{ File = $rel; Role = $(if ($required) { 'sign' } elseif ($skipped) { 'never' } else { 'other' });
                       Status = $signature.Status; Signer = $subject; Timestamped = [bool]$signature.TimeStamperCertificate; Problem = $problem }
}
$rows | Format-Table -AutoSize | Out-String -Width 250 | Write-Host
if ($failures) { Write-Host "verify_signatures: $failures problem(s)"; exit 1 }
Write-Host 'verify_signatures: ok'
