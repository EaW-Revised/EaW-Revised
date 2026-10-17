<#
.SYNOPSIS
  Starts the worker watchdog hidden and detached. Does nothing when it already runs.
.PARAMETER Config
  Config JSON (default tools/watchdog/config.local.json next to this script when it exists).
.PARAMETER Python
  Python 3.11+ executable (default: python from PATH).
#>
param([string]$Config, [string]$Python, [switch]$Foreground)
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'worker_watchdog.py'
if (-not $Config) {
    $local = Join-Path $PSScriptRoot 'config.local.json'
    if (Test-Path -LiteralPath $local) { $Config = $local }
}
if (-not $Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
$arguments = @($script)
if ($Config) { $arguments += @('--config', $Config) }
if ($Foreground) { & $Python @arguments; return }

# Ask python for the output directory so the logs sit next to alerts.md and the pid file.
$outDir = & $Python $script --print-out-dir @($arguments | Select-Object -Skip 1)
if ($LASTEXITCODE -ne 0 -or -not $outDir) { throw 'Could not read the watchdog config.' }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$quoted = $arguments | ForEach-Object { '"' + ($_ -replace '(\*)"', '$1$1\"' -replace '(\+)$', '$1$1') + '"' }
$process = Start-Process -FilePath $Python -ArgumentList $quoted -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $outDir 'service.out.log') -RedirectStandardError (Join-Path $outDir 'service.err.log')
Start-Sleep -Seconds 3
if ($process.HasExited) {
    $text = (Get-Content -LiteralPath (Join-Path $outDir 'service.out.log') -Raw -ErrorAction SilentlyContinue)
    if ($process.ExitCode -eq 3) { Write-Host "Watchdog is already running. $text"; return }
    throw "Watchdog exited with code $($process.ExitCode). See $outDir\service.err.log"
}
Write-Host "Watchdog started (process $($process.Id)); alerts: $outDir\alerts.md"
