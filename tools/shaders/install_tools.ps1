param(
    [string]$DestinationRoot = "out/tools"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path ".").Path
$destination = [System.IO.Path]::GetFullPath((Join-Path $root $DestinationRoot))
$allowed = [System.IO.Path]::GetFullPath((Join-Path $root "out/tools"))
$relativeDestination = [System.IO.Path]::GetRelativePath($allowed, $destination)
if ([System.IO.Path]::IsPathRooted($relativeDestination) -or
    $relativeDestination -eq ".." -or $relativeDestination.StartsWith("..$([System.IO.Path]::DirectorySeparatorChar)")) {
    throw "Destination must stay under $allowed"
}

$contract = Get-Content -Raw (Join-Path $root "tools/shaders/toolchain.json") | ConvertFrom-Json
$glslangDir = Join-Path $destination "glslang-16.5.0"
$archive = Join-Path $destination "glslang-16.5.0.zip"
New-Item -ItemType Directory -Force $destination | Out-Null
if (-not (Test-Path $archive)) {
    Invoke-WebRequest `
        -Uri $contract.glslang.download_url `
        -OutFile $archive
}
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$expected = $contract.glslang.archive_sha256
if ($actual -ne $expected) { throw "glslang archive hash mismatch: $actual" }
if (-not (Test-Path (Join-Path $glslangDir "bin/glslang.exe"))) {
    Expand-Archive -LiteralPath $archive -DestinationPath $glslangDir
}

$toolsSource = Join-Path $destination "SPIRV-Tools-v2026.1"
$headersSource = Join-Path $toolsSource "external/spirv-headers"
$toolsBuild = Join-Path $destination "SPIRV-Tools-v2026.1-build"
if (-not (Test-Path (Join-Path $glslangDir "bin/spirv-val.exe"))) {
    if (-not (Test-Path $toolsSource)) {
        git clone https://github.com/KhronosGroup/SPIRV-Tools.git $toolsSource
        git -C $toolsSource checkout fbe4f3ad913c44fe8700545f8ffe35d1382b7093
    }
    if (-not (Test-Path $headersSource)) {
        git clone https://github.com/KhronosGroup/SPIRV-Headers.git $headersSource
        git -C $headersSource checkout 04f10f650d514df88b76d25e83db360142c7b174
    }
    cmake -S $toolsSource -B $toolsBuild -G "Visual Studio 17 2022" -A x64 `
        -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON
    cmake --build $toolsBuild --config Release --target spirv-val --parallel 4
    Copy-Item -LiteralPath (Join-Path $toolsBuild "tools/Release/spirv-val.exe") `
        -Destination (Join-Path $glslangDir "bin/spirv-val.exe")
}

$toolsCommit = (git -C $toolsSource rev-parse HEAD).Trim()
$headersCommit = (git -C $headersSource rev-parse HEAD).Trim()
if ($toolsCommit -ne $contract.spirv_tools.source_commit) {
    throw "SPIRV-Tools source commit mismatch: $toolsCommit"
}
if ($headersCommit -ne $contract.spirv_tools.spirv_headers_commit) {
    throw "SPIRV-Headers source commit mismatch: $headersCommit"
}

$glslangExe = Join-Path $glslangDir "bin/glslang.exe"
$spirvValExe = Join-Path $glslangDir "bin/spirv-val.exe"
$glslangHash = (Get-FileHash -LiteralPath $glslangExe -Algorithm SHA256).Hash.ToLowerInvariant()
$spirvValHash = (Get-FileHash -LiteralPath $spirvValExe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($glslangHash -ne $contract.glslang.executable_sha256.'windows-x86_64') {
    throw "glslang executable hash mismatch: $glslangHash"
}
if ($spirvValHash -ne $contract.spirv_tools.executable_sha256.'windows-x86_64') {
    throw "spirv-val executable hash mismatch: $spirvValHash"
}

$glslangVersion = (& $glslangExe --version | Select-Object -First 1)
$spirvValVersion = (& $spirvValExe --version | Select-Object -First 1)
if ($glslangVersion -notmatch "^Glslang Version: [0-9]+:$([regex]::Escape($contract.glslang.version))$") {
    throw "glslang version mismatch: $glslangVersion"
}
$commitPrefix = $contract.spirv_tools.source_commit.Substring(0, 7)
if ($spirvValVersion -notmatch "^SPIRV-Tools $([regex]::Escape($contract.spirv_tools.version)) $([regex]::Escape($contract.spirv_tools.version))-[0-9]+-g$commitPrefix$") {
    throw "spirv-val version/commit mismatch: $spirvValVersion"
}

$receipt = [ordered]@{
    schema_version = 1
    platform = "windows-x86_64"
    glslang = [ordered]@{
        archive_sha256 = $actual
        executable_sha256 = $glslangHash
        version = $contract.glslang.version
    }
    spirv_tools = [ordered]@{
        executable_sha256 = $spirvValHash
        source_commit = $toolsCommit
        spirv_headers_commit = $headersCommit
        version = $contract.spirv_tools.version
    }
}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $glslangDir "install-provenance.json") -Encoding utf8NoBOM
$glslangVersion
$spirvValVersion
