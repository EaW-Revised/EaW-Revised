# Build and rig staging share the compatibility identity from the viewer pins.
function Get-EawrProbeCompatibility {
    param([string]$Repo)
    $pins = Get-Content -LiteralPath (Join-Path $Repo 'apps/viewer/dependencies.json') -Raw | ConvertFrom-Json
    [pscustomobject]@{
        GodotVersion = [string]$pins.godot.version
        ViewerAbi = "godot-cpp:$($pins.godot_cpp.commit):api:$($pins.godot_cpp.api_version):gdextension:$($pins.gdextension_compatibility_minimum)"
    }
}

function Write-EawrProbeArtifact {
    param([string]$Library, [string]$SourceCommit, [string]$Repo)
    if ($SourceCommit -notmatch '^[a-f0-9]{40}$') { throw 'Invalid probe build commit' }
    $compatibility = Get-EawrProbeCompatibility -Repo $Repo
    $receipt = [ordered]@{
        schemaVersion = 1; filename = [IO.Path]::GetFileName($Library); commit = $SourceCommit
        sha256 = (Get-FileHash -LiteralPath $Library -Algorithm SHA256).Hash.ToLowerInvariant()
        godotVersion = $compatibility.GodotVersion; viewerAbi = $compatibility.ViewerAbi
    }
    $receipt | ConvertTo-Json | Set-Content -LiteralPath "$Library.json" -Encoding utf8
    [pscustomobject]$receipt
}
