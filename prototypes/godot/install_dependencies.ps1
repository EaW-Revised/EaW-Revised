param([string]$OutputRoot = "")

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (-not $OutputRoot) { $OutputRoot = Join-Path $root "out/godot" }
$deps = Join-Path $OutputRoot "deps"
$bin = Join-Path $OutputRoot "bin"
New-Item -ItemType Directory -Force -Path $deps, (Join-Path $bin "windows"), (Join-Path $bin "linux") | Out-Null

$release = "https://github.com/godotengine/godot/releases/download/4.7.2-stable"
$archives = @(
    @{ Name = "Godot_v4.7.2-stable_win64.exe.zip"; Sha512 = "83decd58fdf67b9d657958a1ae6bf1929c20785315a81effe245874cdc57acb709bf868e00778a96984338c1b29dafdb453c6847747694621c6ecf5da2259993" },
    @{ Name = "Godot_v4.7.2-stable_linux.x86_64.zip"; Sha512 = "9aa00f7a605200940bce3027a567b782f49bd8e940dd06ae9e987bd65aee1b1467edd56ed84fcdcbdd44354bf613bdbb4e5d2913e925850368e150c59ed54c65" }
)
foreach ($archive in $archives) {
    $path = Join-Path $deps $archive.Name
    if (-not (Test-Path $path)) { Invoke-WebRequest "$release/$($archive.Name)" -OutFile $path }
    $actual = (Get-FileHash $path -Algorithm SHA512).Hash.ToLowerInvariant()
    if ($actual -ne $archive.Sha512) { throw "SHA-512 mismatch for $($archive.Name)" }
}

if (-not (Test-Path (Join-Path $deps "godot-cpp/.git"))) {
    git clone --depth 1 --branch 10.0.0-stable https://github.com/godotengine/godot-cpp.git (Join-Path $deps "godot-cpp")
}
$cppCommit = (git -C (Join-Path $deps "godot-cpp") rev-parse HEAD).Trim()
if ($cppCommit -ne "507ed9d840c01a3c5b2a39af8bb4000bfac30bf5") { throw "godot-cpp commit mismatch" }

if (-not (Test-Path (Join-Path $deps "godot-engine/.git"))) {
    git clone --depth 1 --branch 4.7.2-stable https://github.com/godotengine/godot.git (Join-Path $deps "godot-engine")
}
$engineCommit = (git -C (Join-Path $deps "godot-engine") rev-parse HEAD).Trim()
if ($engineCommit -ne "ed1daf0bf001b61586d9930840f2f1394092c079") { throw "Godot engine commit mismatch" }

Expand-Archive (Join-Path $deps $archives[0].Name) (Join-Path $bin "windows") -Force
Expand-Archive (Join-Path $deps $archives[1].Name) (Join-Path $bin "linux") -Force
Write-Host "Pinned Godot 4.7.2 and godot-cpp 10.0.0 are ready below $OutputRoot"
