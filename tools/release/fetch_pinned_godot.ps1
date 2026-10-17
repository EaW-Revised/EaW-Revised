# Download the pinned official Godot 4.7.2 Windows GUI runtime for the player and debug archives.
# The zip is checked against its published SHA-512 (the same pin as prototypes/godot/install_dependencies.ps1);
# tools/release/package_release.py then checks the extracted exe's own SHA-256 and `--version`. Writes only the
# GUI exe (never the console companion) to <Out>\ and prints its path.
param([string]$Out = "out/godot-runtime")

$ErrorActionPreference = "Stop"
$release = "https://github.com/godotengine/godot/releases/download/4.7.2-stable"
$name = "Godot_v4.7.2-stable_win64.exe"
$sha512 = "83decd58fdf67b9d657958a1ae6bf1929c20785315a81effe245874cdc57acb709bf868e00778a96984338c1b29dafdb453c6847747694621c6ecf5da2259993"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$zip = Join-Path $Out "$name.zip"
if (-not (Test-Path -LiteralPath $zip)) { Invoke-WebRequest "$release/$name.zip" -OutFile $zip }
$actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA512).Hash.ToLowerInvariant()
if ($actual -ne $sha512) { throw "SHA-512 mismatch for $name.zip: $actual" }

$extract = Join-Path $Out "extract"
Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
$exe = Join-Path $Out $name
Copy-Item -LiteralPath (Join-Path $extract $name) -Destination $exe -Force
Remove-Item -LiteralPath $extract -Recurse -Force
(Resolve-Path -LiteralPath $exe).Path
