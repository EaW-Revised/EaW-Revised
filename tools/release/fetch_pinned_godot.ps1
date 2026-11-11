# Download the pinned patched Godot 4.7.2 Windows GUI runtime (EAWR fork build, third_party/godot/README.md) for the
# player and debug archives. The zip is checked against its SHA-512 pin (the fork release's SHA512-SUMS.txt);
# tools/release/package_release.py then checks the extracted exe's own SHA-256 and `--version`. Writes only the
# GUI exe (never the console companion) to <Out>\ and prints its path.
# -Console also fetches the console companion (pinned the same way) beside it: the rig/preview/perf engine folder
# (engine guard, tools/engine_pins.json): fetch_pinned_godot.ps1 -Console -Out out\godot-eawr
param([string]$Out = "out/godot-runtime", [switch]$Console)

$ErrorActionPreference = "Stop"
$release = "https://github.com/Thom-Ernst/godot/releases/download/eawr-4.7.2-5b733a90f"
$name = "Godot_v4.7.2-stable_win64.exe"      # the exe inside the zip, and the name the player folder keeps
$asset = "Godot_v4.7.2-stable-eawr-5b733a90f_win64.exe.zip"
$sha512 = "0fbb1b1daf97e1c19c6fbad9dd8ed6f5f73e9dc64b0bfc5db23a6f84cdeaef7f602d424327d7ed5a21d8731e5ef3aca49cb9128a8f6adef064e555a9b3fd1da4"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
function Get-PinnedAsset([string]$asset, [string]$sha512, [string]$name) {
    $zip = Join-Path $Out $asset
    if (-not (Test-Path -LiteralPath $zip)) { Invoke-WebRequest "$release/$asset" -OutFile $zip }
    $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA512).Hash.ToLowerInvariant()
    if ($actual -ne $sha512) { throw "SHA-512 mismatch for ${asset}: $actual" }
    $extract = Join-Path $Out "extract"
    Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
    $exe = Join-Path $Out $name
    Copy-Item -LiteralPath (Join-Path $extract $name) -Destination $exe -Force
    Remove-Item -LiteralPath $extract -Recurse -Force
    (Resolve-Path -LiteralPath $exe).Path
}
$gui = Get-PinnedAsset $asset $sha512 $name
if ($Console) {
    $null = Get-PinnedAsset "Godot_v4.7.2-stable-eawr-5b733a90f_win64_console.exe.zip" "caa0c59b5eaa18447edea597c7c53fdc7444ec509562839c4b9e977c2b293fd937e914e2eafcd1580e9c8a9d109b22085fc42f87f72ca54fa6596b298caf8c94" "Godot_v4.7.2-stable_win64_console.exe"
}
$gui
