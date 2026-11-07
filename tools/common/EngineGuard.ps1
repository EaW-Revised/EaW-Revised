<#
.SYNOPSIS
Engine guard (owner rule 11-03): measurements, gates and previews run only on OUR patched Godot.

.DESCRIPTION
PowerShell twin of tools/common/engine_guard.py. tools/engine_pins.json lists the SHA-256 of every patched exe we accept
('engines') and of known unpatched builds ('known_unpatched', refused with the message
'official Godot detected, not our patched engine'). The only bypass: EAWR_ALLOW_UNPATCHED_ENGINE=ab-control (exact
value), a labelled control arm of an engine A/B; the guard's own tests use the same value.
#>

$script:EawrEnginePinsPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'engine_pins.json'
$script:EawrEngineGuardOverrides = @('ab-control')

function Get-EawrEnginePins {
    Get-Content -LiteralPath $script:EawrEnginePinsPath -Raw | ConvertFrom-Json
}

function Test-EawrEngineGuardOverride {
    <# The bypass value when EAWR_ALLOW_UNPATCHED_ENGINE is exactly ab-control, else $null. #>
    $value = [string]$env:EAWR_ALLOW_UNPATCHED_ENGINE
    if ($script:EawrEngineGuardOverrides -ccontains $value) { $value } else { $null }
}

function Get-EawrEngineFilePins {
    <# @{ <file name> = <sha256> } of a patched engine label (default: the pins file's default). #>
    param([string]$Label)
    $pins = Get-EawrEnginePins
    if (-not $Label) { $Label = [string]$pins.default }
    $entry = $pins.engines.PSObject.Properties[$Label]
    if (-not $entry) { throw "tools/engine_pins.json has no patched engine '$Label'" }
    $files = @{}
    foreach ($property in $entry.Value.files.PSObject.Properties) { $files[$property.Name] = [string]$property.Value }
    $files
}

function Get-EawrEngineHashLabel {
    <# [pscustomobject]@{ label; patched } for a SHA-256; label $null when no table names it. #>
    param([Parameter(Mandatory)][string]$Sha256)
    $pins = Get-EawrEnginePins
    foreach ($table in @(@{ patched = $true; set = $pins.engines }, @{ patched = $false; set = $pins.known_unpatched })) {
        foreach ($entry in $table.set.PSObject.Properties) {
            if (@($entry.Value.files.PSObject.Properties.Value) -contains $Sha256.ToLowerInvariant()) {
                return [pscustomobject]@{ label = $entry.Name; patched = $table.patched }
            }
        }
    }
    [pscustomobject]@{ label = $null; patched = $false }
}

function Get-EawrEngineIdentity {
    param([Parameter(Mandatory)][string]$Path)
    $sha = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $known = Get-EawrEngineHashLabel -Sha256 $sha
    [pscustomobject]@{ path = $Path; sha256 = $sha; label = $known.label; patched = $known.patched; override = $null }
}

function Get-EawrExpectedEngineSha256 {
    <# The pinned hash a refused exe should have had: the default engine's file of the same name, else its GUI exe. #>
    param([string]$FileName)
    $files = Get-EawrEngineFilePins
    if ($FileName -and $files.ContainsKey($FileName)) { return $files[$FileName] }
    if ($files.ContainsKey('Godot_v4.7.2-stable_win64.exe')) { return $files['Godot_v4.7.2-stable_win64.exe'] }
    'unknown'
}

function Get-EawrEngineRefusal {
    <# The refusal text for an unpatched identity (same wording as engine_guard.py). #>
    param([Parameter(Mandatory)]$Identity, [string]$Purpose = 'run')
    $reason = if ($Identity.label) { 'official Godot detected, not our patched engine' } else { 'unknown Godot build, not our patched engine' }
    $named = if ($Identity.label) { " ($($Identity.label))" } else { '' }
    $expected = Get-EawrExpectedEngineSha256 -FileName ([IO.Path]::GetFileName([string]$Identity.path))
    "${reason}: UNPATCHED ENGINE: found sha256 $($Identity.sha256) expected $expected (tools/engine_pins.json); $($Identity.path)$named; $Purpose refused."
}

function Assert-EawrPatchedEngine {
    <# Returns the identity of a pinned patched exe; throws for anything else unless ab-control is set (then override = 'ab-control'). #>
    param([Parameter(Mandatory)][string]$Path, [string]$Purpose = 'run')
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "engine not found: $Path" }
    $identity = Get-EawrEngineIdentity -Path $Path
    if ($identity.patched) { return $identity }
    if (Test-EawrEngineGuardOverride) {
        [Console]::Error.WriteLine("WARNING: engine guard: EAWR_ALLOW_UNPATCHED_ENGINE=$(Test-EawrEngineGuardOverride): allowing $(if ($identity.label) { $identity.label } else { 'unknown' }) $Path ($Purpose); label every result of this run")
        $identity.override = Test-EawrEngineGuardOverride
        return $identity
    }
    throw (Get-EawrEngineRefusal -Identity $identity -Purpose $Purpose)
}
