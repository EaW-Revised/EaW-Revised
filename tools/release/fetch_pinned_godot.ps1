# Download the pinned patched Godot 4.7.2 Windows GUI runtime (EAWR fork build, third_party/godot/README.md) for the
# player and debug archives. The zip is checked against its SHA-512 pin (the fork release's SHA512-SUMS.txt);
# tools/release/package_release.py then checks the extracted exe's own SHA-256 and `--version`. Writes only the
# GUI exe (never the console companion) to <Out>\ and prints its path.
param([string]$Out = "out/godot-runtime")

$ErrorActionPreference = "Stop"
$release = "https://github.com/Thom-Ernst/godot/releases/download/eawr-4.7.2-fad06b579"
$name = "Godot_v4.7.2-stable_win64.exe"      # the exe inside the zip, and the name the player folder keeps
$asset = "Godot_v4.7.2-stable-eawr-fad06b579_win64.exe.zip"
$sha512 = "16b255f4b50ebe4fca9cbb89b53ec06e1f625b905e78f6effa886359f7bbdceaa00e7ee27a4c6814c1a8ae8a11f5b5a8fab5e0e04bc9efefdc18b3f682f8d97d"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
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
