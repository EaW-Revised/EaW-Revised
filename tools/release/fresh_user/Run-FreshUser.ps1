# Fresh-user gate for one release archive (player or debug) on a GPU rig host, through the normal rig queue.
#
#   pwsh -NoProfile -File tools/release/fresh_user/Run-FreshUser.ps1 -Package <zip> -Kind player|debug `
#        -ExpectedSha256 <sha256 of the zip> -Machine <GPU host name or alias from config/gpu-hosts.json> [-Ref <release commit>]
#
# 1. Checks the zip against -ExpectedSha256 and stages it on the host (hash verified there; the extra-artifact route of the
#    rig suites is limited to 256 MiB and the Debug archive is larger).
# 2. Builds a clean one-commit checkout of -Ref (default HEAD) in out/fresh-user-src/<id>: the release commit's tree plus
#    fresh_user_player.py as tests/presentation/renderer/test_release_player_fresh_user.py and the archive's viewer extension in
#    apps/viewer/project/bin (ignored), because the rig suite preflight refuses a dirty checkout. The suite itself only
#    launches the EXTRACTED archive (its own EAWR.exe, bundled Godot, project and extension).
# 3. Runs that checkout's tools/rig/Invoke-RigSuite.ps1 (queue, private desktop, GPU check) with EAWR_JOB_RELEASE_PACKAGE / _KIND /
#    _OUT pointing at the staged zip and copies the result folder back to out/rig-suite/<run> of this worktree.
# Exit code = Invoke-RigSuite's. Receipts: <run>/summary.json, suites.log, tmp/**/fresh-user-receipt.json.
param(
    [Parameter(Mandatory)][string]$Package, [ValidateSet('player', 'debug')][string]$Kind = 'player',
    [Parameter(Mandatory)][string]$ExpectedSha256, [Parameter(Mandatory)][string]$Machine, [string]$Ref = 'HEAD',
    [int]$TimeoutSeconds = 5400, [int]$QueueTimeoutSeconds = 21600, [string]$Harness, [string]$Repo
)
$ErrorActionPreference = 'Stop'
# The rig scripts copy files with scp. Git for Windows ships an MSYS scp that reads "C:\..." as a remote host; use Windows OpenSSH.
$env:PATH = "$env:SystemRoot\System32\OpenSSH;$env:PATH"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = if ($Repo) { [IO.Path]::GetFullPath($Repo) } else { [IO.Path]::GetFullPath((Join-Path $here '..\..\..')) }
if (-not $Harness) { $Harness = Join-Path $here 'fresh_user_player.py' }
$expected = & python (Join-Path $here 'count_tests.py') $Harness
if ($LASTEXITCODE -ne 0 -or "$expected" -notmatch '^[1-9][0-9]*$') { throw 'could not collect fresh-user test count' }
$expected = [int]$expected
$Package = (Resolve-Path -LiteralPath $Package).Path
$actual = (Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $ExpectedSha256.ToLowerInvariant()) { throw "Package sha256 $actual does not match the expected $ExpectedSha256" }
$commit = (& git -C $repoRoot rev-parse "$Ref^{commit}").Trim()
. (Join-Path $repoRoot 'tools\common\EawrHostPaths.ps1')
. (Join-Path $repoRoot 'tools\rig\RigCommon.ps1')
. (Join-Path $repoRoot 'tools\rig\RigQueue.ps1')
. (Join-Path $repoRoot 'tools\rig\RigHosts.ps1')
$table = Get-RigHostTable
$cand = @(Select-RigHost -Table $table -Machine @($Machine) -Kind suite)
if ($cand.Count -ne 1) { throw "expected exactly one host for -Machine $Machine, got $($cand.Count)" }
if ($cand[0].Connection -eq 'local') { throw 'never the local workstation' }
$savedOffloadOnly = $env:EAWR_OFFLOAD_ONLY
# The staged archive and the checkout are removed on every exit (success, failed verdict or an error part-way) once the
# evidence is fetched; if the suite ran but its evidence could not be copied back, the checkout and host folder stay for diagnosis.
$sessions = $null; $dest = $null; $work = $null; $suiteStarted = $false; $evidenceFetched = $false; $localRun = $null
try {
    $sessions = Open-RigHostSession -Candidates $cand
    $s = $sessions[$cand[0].Name]
    $id = (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Kind"
    $dest = "$(Get-RigLabRoot $s)\fresh\$id"
    $name = Split-Path -Leaf $Package
    Invoke-RigHostCommand -Session $s -ArgumentList $dest -ScriptBlock { param($d) New-Item -ItemType Directory -Force -Path $d | Out-Null }
    Write-Host "staging $name to $($cand[0].Name):$dest"
    # scp reads a drive-letter path as a remote host, so hand it a forward-slash path relative to the zip's own folder.
    Push-Location (Split-Path -Parent $Package)
    try { Copy-RigToHost -Session $s -Path "./$name" -Destination "$dest\$name" } finally { Pop-Location }
    $remoteHash = Invoke-RigHostCommand -Session $s -ArgumentList "$dest\$name" -ScriptBlock { param($p) (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }
    if ("$remoteHash" -ne $actual) { throw "staged zip hash $remoteHash differs from $actual" }
    Close-RigHostSession $sessions

    # clean one-commit checkout: the release tree + the suite file; the archive's extension satisfies the preflight
    $work = Join-Path $repoRoot "out\fresh-user-src\$id"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $zipTree = Join-Path (Split-Path $work) "$id.tree.zip"
    & git -C $repoRoot archive --format=zip -o $zipTree $commit
    if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }
    Expand-Archive -LiteralPath $zipTree -DestinationPath $work -Force
    Remove-Item -LiteralPath $zipTree -Force
    $suite = 'tests/presentation/renderer/test_release_player_fresh_user.py'
    Copy-Item -LiteralPath $Harness -Destination (Join-Path $work $suite) -Force
    & git -C $work init -q
    & git -C $work config core.longpaths true
    & git -C $work add -A
    if ($LASTEXITCODE -ne 0) { throw 'git add failed in the harness checkout' }
    & git -C $work -c user.name=release -c user.email=release@example.invalid commit -q -m "fresh-user harness checkout of $commit"
    $bin = Join-Path $work 'apps\viewer\project\bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($Package)
    try {
        $e = $z.Entries | Where-Object { $_.FullName -like '*/project/bin/libeawr_viewer.windows.template_release.x86_64.dll' } | Select-Object -First 1
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $bin 'libeawr_viewer.windows.template_release.x86_64.dll'), $true)
    } finally { $z.Dispose() }
    $envArgs = @("EAWR_JOB_RELEASE_PACKAGE=$dest\$name", "EAWR_JOB_RELEASE_KIND=$Kind", "EAWR_JOB_RELEASE_OUT=$dest\out")
    $suiteParams = @{ Suite = $suite; Machine = @($Machine); TimeoutSeconds = $TimeoutSeconds; QueueTimeoutSeconds = $QueueTimeoutSeconds
                      PrivateDesktop = $true; Env = $envArgs }
    Write-Host ("Invoke-RigSuite (checkout $work) " + (($suiteParams.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value -join ';')" }) -join ' '))
    # In-process call with a splatted hashtable: -File would not bind an array to -Env.
    $global:LASTEXITCODE = 0
    $suiteStarted = $true
    # While the owner reserves the workstation Invoke-RigSuite would queue a remote viewer build of this checkout; the suite
    # needs none (it launches the extracted archive), so use the extension staged above.
    $env:EAWR_OFFLOAD_ONLY = '0'
    & (Join-Path $work 'tools\rig\Invoke-RigSuite.ps1') @suiteParams
    $suiteExit = $LASTEXITCODE
    $runs = Join-Path $work 'out\rig-suite'
    $target = Join-Path $repoRoot 'out\rig-suite'
    $localRun = $null
    if (Test-Path -LiteralPath $runs) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        $last = Get-ChildItem $runs -Directory | Where-Object { $_.Name -notlike 'preflight*' } | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($last) { Copy-Item -LiteralPath $last.FullName -Destination $target -Recurse -Force; $localRun = Join-Path $target $last.Name }
    }
    # The fresh-user receipt and the launch consoles are written by the suite on the host (EAWR_JOB_RELEASE_OUT); fetch them.
    $sessions = Open-RigHostSession -Candidates $cand
    $s = $sessions[$cand[0].Name]
    $hostZip = Invoke-RigHostCommand -Session $s -ArgumentList $dest -ScriptBlock {
        param($d)
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $zipPath = "$d\out.zip"
        Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue
        $zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
        try {
            foreach ($f in Get-ChildItem -LiteralPath "$d\out" -Recurse -File -ErrorAction SilentlyContinue) {
                if ($f.Extension -notin '.json', '.log', '.txt') { continue }
                if ($f.Length -gt 32MB) { continue }
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $f.FullName.Substring($d.Length + 5).Replace('\', '/'), 'Fastest')
            }
        } finally { $zip.Dispose() }
        $zipPath
    }
    $evidence = Join-Path ($(if ($localRun) { $localRun } else { $target })) 'fresh-out'
    New-Item -ItemType Directory -Force -Path $evidence | Out-Null
    Copy-RigFromHost -Session $s -Path "$hostZip" -Destination (Join-Path $evidence 'fresh-out.zip')
    Close-RigHostSession $sessions
    $evidenceFetched = $true
    [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $evidence 'fresh-out.zip'), (Join-Path $evidence 'files'))
    # Verdict: all nine cases passed, the receipt names the staged archive, and the host GPU shows in the launch evidence.
    $verdict = [ordered]@{ kind = $Kind; archive = $name; archive_sha256 = $actual; machine = $cand[0].Name; suite_exit = $suiteExit; commit = $commit }
    $summaryPath = if ($localRun) { Join-Path $localRun 'summary.json' } else { $null }
    $passed = 0; $failed = 0
    if ($summaryPath -and (Test-Path -LiteralPath $summaryPath)) {
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        foreach ($t in $summary.suites[0].tests) { if ($t.outcome -eq 'passed') { $passed++ } else { $failed++ } }
    }
    $receiptPath = Join-Path $evidence 'files\fresh-user-receipt.json'
    $receiptOk = $false
    if (Test-Path -LiteralPath $receiptPath) {
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
        $receiptOk = ($receipt.archive_sha256 -eq $actual) -and ($receipt.kind -eq $Kind)
    }
    $adapter = [string]$cand[0].Adapter
    $gpuSeen = 0
    if ($localRun -and $adapter) {
        $gpuSeen = @(Get-ChildItem -LiteralPath $localRun -Recurse -File -Include *.json, *.log, *.txt -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -lt 8MB -and (Select-String -LiteralPath $_.FullName -SimpleMatch $adapter -Quiet) }).Count
    }
    $verdict.cases_passed = $passed; $verdict.cases_failed = $failed; $verdict.receipt_matches_archive = $receiptOk
    $verdict.adapter = $adapter; $verdict.files_naming_adapter = $gpuSeen
    $verdict.expected_cases = $expected
    $verdict.result = if ($passed -eq $expected -and $failed -eq 0 -and $receiptOk -and $gpuSeen -gt 0) { "PASS $passed/$expected" } else { 'FAIL' }
    ($verdict | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath (Join-Path $evidence 'fresh-user-verdict.json') -Encoding utf8
    Write-Host ("fresh-user {0}: {1} (cases {2} passed / {3} failed, receipt ok {4}, adapter named in {5} files; Invoke-RigSuite exit {6}); {7}" -f $Kind, $verdict.result, $passed, $failed, $receiptOk, $gpuSeen, $suiteExit, $evidence)
} finally {
    Close-RigHostSession $sessions
    if ($dest -and ($evidenceFetched -or -not $suiteStarted)) {
        try {
            $sessions = Open-RigHostSession -Candidates $cand
            Invoke-RigHostCommand -Session $sessions[$cand[0].Name] -ArgumentList $dest -ScriptBlock { param($d) Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
        } catch { Write-Warning "could not remove the staged folder $dest on the host: $_" } finally { Close-RigHostSession $sessions }
    }
    if ($work -and (Test-Path -LiteralPath $work) -and ($localRun -or -not $suiteStarted)) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    $env:EAWR_OFFLOAD_ONLY = $savedOffloadOnly
}
exit $(if ($verdict.result -eq 'PASS 9/9') { 0 } else { 1 })
