# Debug root tools + online-enabled Debug viewer for the Debug archive (docs/releasing.md "The debug folder"), on one pool host.
# Maintainer tooling: run as the build user over ssh, after the exact commit was pushed to <Base>\repo.git as refs/release-kit/debug
# (git push <build user>@<host>:<repo path> <sha>:refs/release-kit/debug).
# Takes one of the host's build slot locks (the same lock files as windows_remote_build.ps1) for the whole run, so the pool never
# overcommits the machine, and builds in its own tree <Base>\release-kit\debug\src (the slot trees stay untouched). Never run it on
# the host that builds the Release viewer for the same release (the two viewers must not share an output tree or a machine's slots).
param([Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$Sha, [string]$Jobs = '4', [string]$Slots = '1',
      [string]$LockTimeout = '21600', [string]$KitName = 'release-kit', [string]$RootBuild = 'release-debug')
$ErrorActionPreference = 'Stop'
function Say([string]$t) { [Console]::Out.WriteLine($t); [Console]::Out.Flush() }
if ($Sha -notmatch '^[0-9a-f]{40}$') { throw 'bad sha' }
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch { }
$env:MSBUILDDISABLENODEREUSE = '1'; $env:VSCMD_SKIP_SENDTELEMETRY = '1'; $env:VSLANG = '1033'
$kit = Join-Path $Base $KitName
$logs = Join-Path $kit "logs\debug-$($Sha.Substring(0,9))"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$repo = Join-Path $Base 'repo.git'
$src = Join-Path $kit 'debug\src'

$end = (Get-Date).AddSeconds([int]$LockTimeout)
$lock = $null; $slot = -1
while (-not $lock) {
    foreach ($c in 0..([int]$Slots - 1)) {
        $name = if ($c -eq 0) { 'build.lock' } else { "build$c.lock" }
        try { $lock = [IO.File]::Open((Join-Path $Base $name), 'OpenOrCreate', 'ReadWrite', 'None'); $slot = $c; break } catch { }
    }
    if ($lock) { break }
    if ((Get-Date) -ge $end) { Say 'EAWR-RELEASE lock-timeout'; exit 75 }
    Start-Sleep -Seconds 10
}
$ownerName = if ($slot -eq 0) { 'build.owner' } else { "build$slot.owner" }
[IO.File]::WriteAllText((Join-Path $Base $ownerName), "release debug build $Sha since $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))")
Say "EAWR-RELEASE slot $slot locked"

function Run([string]$cmd, [string]$log, [string]$cwd) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $info.Arguments = "/d /s /c `"$cmd < NUL > `"$log`" 2>&1`""
    $info.UseShellExecute = $false; $info.WorkingDirectory = $cwd
    $p = [Diagnostics.Process]::Start($info); $p.WaitForExit(); $p.ExitCode
}
function Stage([string]$name, [string]$cmd, [string]$cwd) {
    $t = Get-Date
    $code = Run $cmd (Join-Path $logs "$name.log") $cwd
    Say ("EAWR-STAGE debug {0} {1} {2}" -f $name, $code, [int]((Get-Date) - $t).TotalSeconds)
    if ($code -ne 0) { Say "EAWR-RELEASE failed at $name"; Get-Content (Join-Path $logs "$name.log") -Tail 40 | ForEach-Object { Say $_ }; exit 1 }
}
try {
    New-Item -ItemType Directory -Force -Path (Split-Path $src) | Out-Null
    $co = @()
    if (-not (Test-Path -LiteralPath (Join-Path $src '.git'))) { $co += "git -C `"$repo`" worktree prune"; $co += "git -C `"$repo`" worktree add --detach `"$src`" $Sha" }
    $co += "git -C `"$src`" checkout -q --force --detach $Sha"
    $co += "if exist `"$src\tools\offload\normalize_checkout.py`" (python `"$src\tools\offload\normalize_checkout.py`" `"$src`")"
    $co += "git -C `"$src`" clean -q -ffdx -e /out/ -e /apps/viewer/project/bin/ -e /apps/viewer/project/common/"
    Stage 'checkout' ('(' + ($co -join ' && ') + ')') $Base
    $head = (& git -C $src rev-parse HEAD).Trim()
    if ($head -ne $Sha) { throw "checkout at $head" }

    $vs = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -version '[17.0,18.0)' -latest -products * -property installationPath | Select-Object -First 1)
    if (-not $vs) { throw 'no Visual Studio 2022' }
    $dev = "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >NUL 2>&1 && "
    $pins = Get-Content -LiteralPath (Join-Path $src 'apps\viewer\dependencies.json') -Raw | ConvertFrom-Json
    $cpp = Join-Path $Base "deps\godot-cpp-$($pins.godot_cpp.commit)"
    if (-not (Test-Path -LiteralPath (Join-Path $cpp 'CMakeLists.txt'))) {
        Stage 'godot-cpp' "git clone -q --depth 1 --branch $($pins.godot_cpp.version) https://github.com/godotengine/godot-cpp.git `"$cpp`"" $Base
        if ((& git -C $cpp rev-parse HEAD).Trim() -ne $pins.godot_cpp.commit) { throw 'godot-cpp pin mismatch' }
    }
    $tools = 'sim_headless','sim_bench','path_bench','asset_scan','asset_validate','xml_scan','scene_scan','sky_scan','unit_scan','script_smoke','hud_movie_prepare'
    # root Debug with the static debug C runtime, tests off, symbols on so each tool has its PDB
    $root = "out\build\$RootBuild"
    Stage 'root-configure' ("($dev" + "cmake --preset windows-msvc-ninja -B $root -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug -DEAWR_WARNINGS_AS_ERRORS=OFF -DBUILD_TESTING=OFF -DEAWR_DEBUG_SYMBOLS=ON)") $src
    Stage 'root-build' ("($dev" + "cmake --build $root --config Debug --target $($tools -join ' ') -j $Jobs)") $src
    # the online-enabled Debug viewer (relay + GNS), in its own tree; bin/ of THIS tree only
    $vdir = 'out\build\viewer-debug-relay-ninja'
    Stage 'viewer-configure' ("($dev" + "cmake -S apps\viewer -B $vdir -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl -DEAWR_GODOT_CPP_ROOT=`"$cpp`" -DEAWR_ENABLE_LAND_SKIRMISH=OFF -DEAWR_DEBUG_SYMBOLS=ON -DEAWR_ENABLE_RELAY=ON -DEAWR_ENABLE_GNS=ON)") $src
    Stage 'viewer-build' ("($dev" + "cmake --build $vdir --config Debug --target eawr_viewer -j $Jobs)") $src
    $dll = Join-Path $src 'apps\viewer\project\bin\libeawr_viewer.windows.template_release.x86_64.dll'
    if (-not (Test-Path -LiteralPath $dll) -or -not (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($dll, '.pdb')))) { throw 'debug viewer DLL or PDB missing' }
    Say ("EAWR-RELEASE viewer-dll {0}" -f (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant())
    Say 'EAWR-RELEASE debug-build-ok'
} finally {
    $lock.Dispose()
}
