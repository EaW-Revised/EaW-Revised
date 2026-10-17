<#
.SYNOPSIS
  Stops the worker watchdog. Only the process recorded in its own pid file, and only after checking that the
  process is still this script (command line) and was created before the pid file said it started.
#>
param([string]$Config, [string]$Python)
$ErrorActionPreference = 'Stop'
if (-not $Config) {
    $local = Join-Path $PSScriptRoot 'config.local.json'
    if (Test-Path -LiteralPath $local) { $Config = $local }
}
if (-not $Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
$script = Join-Path $PSScriptRoot 'worker_watchdog.py'
$arguments = @($script, '--print-out-dir')
if ($Config) { $arguments += @('--config', $Config) }
$outDir = & $Python @arguments
if ($LASTEXITCODE -ne 0 -or -not $outDir) { throw 'Could not read the watchdog config.' }
$pidFile = Join-Path $outDir 'watchdog.pid'
if (-not (Test-Path -LiteralPath $pidFile)) { Write-Host 'No pid file: the watchdog is not running.'; return }
$record = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
$process = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $([int]$record.pid)" -ErrorAction SilentlyContinue
if (-not $process) { Write-Host "Process $($record.pid) is gone; removing the stale pid file."; Remove-Item -LiteralPath $pidFile; return }
$started = [DateTimeOffset]::FromUnixTimeSeconds([long]$record.started_epoch).UtcDateTime
$created = $process.CreationDate.ToUniversalTime()
if ($process.CommandLine -notlike "*$($record.script)*" -or $created -gt $started.AddSeconds(5) -or $created -lt $started.AddMinutes(-2)) {
    Write-Host "Process $($record.pid) is not the watchdog recorded in the pid file (command line or start time differ). Not touching it."
    return
}
Stop-Process -Id $process.ProcessId -Force
Remove-Item -LiteralPath $pidFile -ErrorAction SilentlyContinue
Write-Host "Stopped the watchdog (process $($process.ProcessId))."
