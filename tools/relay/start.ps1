param([string]$Config, [string]$Binary, [string]$LogDirectory)
$ErrorActionPreference = 'Stop'
$relayRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $Config) { $Config = Join-Path $relayRoot 'out/relay/relay.local.conf' }
if (-not $Binary) { $Binary = Join-Path $relayRoot 'out/relay/Release/eawr_relay_server.exe' }
if (-not $LogDirectory) { $LogDirectory = Join-Path $relayRoot 'out/relay/logs' }
$Config = [IO.Path]::GetFullPath($Config)
$Binary = [IO.Path]::GetFullPath($Binary)
$LogDirectory = [IO.Path]::GetFullPath($LogDirectory)
if (-not (Test-Path -LiteralPath $Binary) -or -not (Test-Path -LiteralPath $Config)) {
    throw 'Build the standalone relay and create relay.local.conf as described in docs/deploy/relay-server.md.'
}
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
Push-Location (Split-Path -Parent $Config)
try {
    & $Binary --config $Config 2>&1 | Tee-Object -FilePath (Join-Path $LogDirectory 'relay.log') -Append
    if ($LASTEXITCODE -ne 0) { throw 'Relay exited with an error; see the log.' }
} finally { Pop-Location }
