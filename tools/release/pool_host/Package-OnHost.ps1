# Host-side packaging of a finished pool build (Release) or of the Debug build made by Build-DebugOnHost.ps1.
# Maintainer tooling: run as the build user on a pool host (over ssh), after the producer job finished.
#
#   -Kind release   the pool slot tree <Base>\src[<Slot>] built by windows_build.py --viewer --preset windows-msvc-ninja-relay
#                   --require-symbols (root tools + launcher + mods service + load check, and the viewer artifact zip).
#   -ViewerTree     optional: another slot's source tree that holds the built relay viewer (bin/, common/); use it when the root tree
#                   holds an older viewer build (the viewer zip carries no PDB, so a stale one would not match the new DLL)
#   -Kind debug     the tree <Base>\release-kit\debug\src made by Build-DebugOnHost.ps1; the launcher, mods service and load check
#                   come from -InputsDir (the Release ones, same bytes as in the player zip).
#
# Stages a clean copy of the source tree (the slot is not touched afterwards), then runs tools/release/package_release.py there with
# the pinned patched Godot (-Godot), the pinned manager artifact (-LauncherArtifact) and --with-mods-manager. Prints
# "EAWR-RELEASE FILE <path> <bytes> <sha256>" for every archive. The private symbol archive goes to <Base>\release-kit\symbols-<kind>.
#   -NoSymbols      owner TEST kit: skip private symbol retention (package_release.py --no-symbols); refused with -Public.
#   -BuildDir       defaults to out\build\windows-msvc-ninja-relay, the preset the release tree is built with; -Kind debug needs
#                   -BuildDir out\build\release-debug (where Build-DebugOnHost.ps1 builds).
param(
    [Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$Sha, [Parameter(Mandatory)][ValidateSet('release', 'debug')][string]$Kind,
    [Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$Godot, [Parameter(Mandatory)][string]$LauncherArtifact,
    [string]$Slot = '0', [string]$ViewerZip = '', [string]$Tag = '',
    [string]$RevisedLayer = $env:EAWR_REVISED_LAYER_DIR, [switch]$AllowMissingRevisedLayer, [switch]$Public, [switch]$NoSymbols,
    [string]$BuildDir = 'out\build\windows-msvc-ninja-relay', [string]$InputsDir = '', [string]$KitName = 'release-kit', [string]$ViewerTree = '', [string]$Tree = ''
)
$ErrorActionPreference = 'Stop'
function Say([string]$t) { [Console]::Out.WriteLine($t); [Console]::Out.Flush() }
if ($Sha -notmatch '^[0-9a-f]{40}$') { throw 'bad sha' }
$kit = Join-Path $Base $KitName
if (-not $Tag) { $Tag = "$($Sha.Substring(0,9))-$Kind" }
$stage = Join-Path $kit $Tag
$src = if ($Kind -eq 'release' -and $Tree) { $Tree } elseif ($Kind -eq 'release') { if ($Slot -eq '0') { Join-Path $Base 'src' } else { Join-Path $Base "src$Slot" } } else { Join-Path $kit 'debug\src' }
$head = (& git -C $src rev-parse HEAD).Trim()
if ($head -ne $Sha) { throw "tree $src is at $head, expected $Sha" }
$buildSrc = Join-Path $src $BuildDir
if (-not (Test-Path -LiteralPath $buildSrc -PathType Container)) { throw "build dir not found: $buildSrc (built with --preset windows-msvc-ninja-relay? pass -BuildDir for another preset)" }
if ($NoSymbols -and $Public) { throw '-NoSymbols is for owner test kits and cannot be combined with -Public' }
$tools = 'sim_headless','sim_bench','path_bench','asset_scan','asset_validate','xml_scan','scene_scan','sky_scan','unit_scan','script_smoke','hud_movie_prepare'
# Every expected binary must be in the build tree before anything is staged.
$expected = @(); foreach ($t in $tools) { $expected += "$t.exe", "$t.pdb" }
if ($Kind -eq 'release') { $expected += 'EAWR.exe', 'EAWR Mods.exe', 'eawr_mods_service.exe', 'eawr_loadcheck.exe', 'EAWR.pdb' }
$buildApps = Join-Path $buildSrc 'apps'
if (-not (Test-Path -LiteralPath $buildApps -PathType Container)) { throw "build tree has no apps folder: $buildApps" }
foreach ($file in $expected) {
    $found = @(Get-ChildItem -LiteralPath $buildApps -Recurse -Filter $file -File | Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
    if ($found.Count -ne 1) { throw "expected binary $file : $($found.Count) matches under $buildApps (wrong -BuildDir, or the tree was not built)" }
}
Say "stage $stage from $src"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path "$stage\out" -Force | Out-Null
$null = robocopy $src "$stage\src" /E /XD out .git .godot /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "robocopy failed $LASTEXITCODE" }
$global:LASTEXITCODE = 0
if ($ViewerZip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # the pool's viewer artifact: untracked bin/ and common/ files of the viewer project; overwrite file by file
    $z = [IO.Compression.ZipFile]::OpenRead($ViewerZip)
    try {
        foreach ($e in $z.Entries) {
            if ($e.FullName.EndsWith('/')) { continue }
            $target = Join-Path "$stage\src\apps\viewer\project" ($e.FullName -replace '/', '\')
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        }
    } finally { $z.Dispose() }
}
if ($ViewerTree) {
    # The relay viewer was built in another build slot: take its untracked bin/ (extension, PDB) and common/ files.
    foreach ($sub in 'bin', 'common') {
        $from = Join-Path $ViewerTree (Join-Path 'apps\viewer\project' $sub)
        $to = Join-Path $stage (Join-Path 'src\apps\viewer\project' $sub)
        if (Test-Path -LiteralPath $from) { $null = robocopy $from $to /E /NFL /NDL /NJH /NJS /NP; if ($LASTEXITCODE -ge 8) { throw "robocopy $sub failed" } }
    }
    $global:LASTEXITCODE = 0
}
$bd = Join-Path $stage 'build'
function Copy-Built([string]$name, [string]$ext) {
    $hits = @(Get-ChildItem -LiteralPath (Join-Path $buildSrc 'apps') -Recurse -Filter ($name + $ext) -File | Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
    if ($hits.Count -ne 1) { throw "expected binary $name$ext : $($hits.Count) matches under $buildSrc\apps (wrong -BuildDir, or the tree was not built)" }
    $rel = $hits[0].FullName.Substring($buildSrc.Length + 1)
    $dest = Join-Path $bd $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Copy-Item -LiteralPath $hits[0].FullName -Destination $dest
    return $dest
}
foreach ($t in $tools) { $null = Copy-Built $t '.exe'; $null = Copy-Built $t '.pdb' }
if ($Kind -eq 'release') {
    $launcher = Copy-Built 'EAWR' '.exe'
    $start = Copy-Built 'EAWR Mods' '.exe'
    $service = Copy-Built 'eawr_mods_service' '.exe'
    $loadcheck = Copy-Built 'eawr_loadcheck' '.exe'
    $null = Copy-Built 'EAWR' '.pdb'
    if ((Split-Path $launcher) -ne (Split-Path $start)) { throw 'launcher and mods start stub are not side by side' }
    if ((Split-Path $service) -ne (Split-Path $loadcheck)) { throw 'mods service and load check are not side by side' }
} else {
    # The Debug archive carries the Release launcher and manager binaries (same bytes as the player zip).
    $launcher = Join-Path $InputsDir 'EAWR.exe'
    $service = Join-Path $InputsDir 'eawr_mods_service.exe'
    foreach ($f in $launcher, (Join-Path $InputsDir 'EAWR Mods.exe'), $service, (Join-Path $InputsDir 'eawr_loadcheck.exe')) { if (-not (Test-Path -LiteralPath $f)) { throw "missing $f" } }
}
New-Item -ItemType Directory -Force -Path (Join-Path $bd 'src\core\generated') | Out-Null
Copy-Item -LiteralPath (Join-Path $buildSrc 'src\core\generated\build_identity.cpp') -Destination (Join-Path $bd 'src\core\generated\build_identity.cpp')

$out = Join-Path $stage 'out'
$args = @("$stage\src\tools\release\package_release.py", '--expect-identity', $Sha, '--build-dir', $bd, '--platform', 'windows-x64', '--version', $Version,
    '--out', $out, '--symbol-store', (Join-Path $kit "symbols-$Kind"), '--godot', $Godot, '--launcher', $launcher, '--with-mods-manager',
    '--mods-service', $service, '--launcher-artifact', $LauncherArtifact)
if ($RevisedLayer) { $args += @('--revised-layer', $RevisedLayer) }
if ($AllowMissingRevisedLayer) { $args += '--allow-missing-revised-layer' }
if ($Public) { $args += '--public' }
if ($NoSymbols) { $args += '--no-symbols' }
if ($Kind -eq 'debug') { $args += '--debug' }
Say ("python " + ($args -join ' '))
$log = Join-Path $stage 'package.log'
$ErrorActionPreference = 'Continue'
& python @args *> $log
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Get-Content -LiteralPath $log | ForEach-Object { Say $_ }
if ($code -ne 0) { Say "EAWR-RELEASE package failed $code"; exit $code }
Get-ChildItem -LiteralPath $out -File | ForEach-Object { Say ("EAWR-RELEASE FILE {0} {1} {2}" -f $_.FullName, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()) }
if ($Kind -eq 'release') {
    # the manager binaries the Debug archive reuses
    $inputs = Join-Path $kit "$Tag\inputs"
    New-Item -ItemType Directory -Force -Path $inputs | Out-Null
    foreach ($f in $launcher, $start, $service, $loadcheck) { Copy-Item -LiteralPath $f -Destination $inputs }
    Say "EAWR-RELEASE INPUTS $inputs"
}
Say 'EAWR-RELEASE done'
