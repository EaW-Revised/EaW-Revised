<#
.SYNOPSIS
Resolves host-specific paths for the rig scripts from config/host-paths.json.

.DESCRIPTION
Dot-source this file, then call Resolve-EawrHostPath for each path setting.
Precedence, highest first:

1. an explicit script parameter (passed in as -Explicit);
2. the setting's environment variable (EAWR_RIG_TOOLS_ROOT and so on);
3. the file named by EAWR_HOST_PATHS, else config/host-paths.local.json (ignored);
4. the committed defaults in config/host-paths.json.

A missing layer is skipped. A setting that no layer supplies throws, unless
-Optional is given, in which case $null is returned.
#>

$script:EawrHostPathSettings = [ordered]@{
    rigToolsRoot         = 'EAWR_RIG_TOOLS_ROOT'
    rigPython            = 'EAWR_RIG_PYTHON'
    airtestPython        = 'EAWR_AIRTEST_PYTHON'
    rigAirtestRunnerRoot = 'EAWR_RIG_AIRTEST_RUNNER_ROOT'
    rigFeasibilityRoot   = 'EAWR_RIG_FEASIBILITY_ROOT'
    focMenuTemplates     = 'EAWR_FOC_MENU_TEMPLATES'
    linuxBuildHost       = 'EAWR_LINUX_BUILD_HOST'
    linuxBuildUser       = 'EAWR_LINUX_BUILD_USER'
    linuxBuildIdentity   = 'EAWR_LINUX_BUILD_IDENTITY'
    windowsBuildHost     = 'EAWR_WINDOWS_BUILD_HOST'
    windowsBuildUser     = 'EAWR_WINDOWS_BUILD_USER'
    windowsBuildIdentity = 'EAWR_WINDOWS_BUILD_IDENTITY'
    localGpuSlotTool     = 'EAWR_LOCAL_GPU_SLOT_TOOL'
    localLabRoot         = 'EAWR_LOCAL_LAB_ROOT'
}

function Read-EawrHostPathFile {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $data = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($data.schemaVersion -ne 1) { throw "Unsupported host-paths schemaVersion in ${Path}: $($data.schemaVersion)" }
    foreach ($p in $data.PSObject.Properties) {
        if ($p.Name -in 'schemaVersion', '_comment') { continue }
        if (-not $script:EawrHostPathSettings.Contains($p.Name)) { throw "Unknown host-paths setting '$($p.Name)' in $Path" }
    }
    $data
}

function Resolve-EawrHostPath {
    param(
        [Parameter(Mandatory)][string]$Name,
        [string]$Explicit,
        [switch]$Optional
    )
    if (-not $script:EawrHostPathSettings.Contains($Name)) { throw "Unknown host-paths setting '$Name'" }
    if ($Explicit) { return $Explicit }
    $fromEnv = [Environment]::GetEnvironmentVariable($script:EawrHostPathSettings[$Name])
    if ($fromEnv) { return $fromEnv }

    $configDir = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'config'
    $override = $env:EAWR_HOST_PATHS
    if (-not $override) { $override = Join-Path $configDir 'host-paths.local.json' }
    elseif (-not (Test-Path -LiteralPath $override)) { throw "EAWR_HOST_PATHS names a missing file: $override" }
    foreach ($file in @($override, (Join-Path $configDir 'host-paths.json'))) {
        $data = Read-EawrHostPathFile $file
        if ($data -and $data.PSObject.Properties[$Name] -and $data.$Name) { return [string]$data.$Name }
    }
    if ($Optional) { return $null }
    throw "Host path '$Name' is not configured: pass it explicitly, set $($script:EawrHostPathSettings[$Name]), or add it to config/host-paths.local.json"
}
