param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    # Optional overrides. build.ps1 passes the start-of-build snapshot so the
    # recorded fingerprint and revision are the same moment; a direct CMake/Ninja
    # relink (CMakeLists.txt POST_BUILD) omits them and snapshots at link time.
    [string]$SourceRevision='',
    [string]$SourceFingerprint='',
    [switch]$SourceDirty,
    [string]$Configuration='Release'
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'build-metadata.ps1')

$exe=Join-Path $OutputDirectory 're-ps3-native.exe'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "No re-ps3-native.exe at '$OutputDirectory'."
}

if(-not $SourceFingerprint) { $SourceFingerprint=Get-NativeSourceFingerprint -ProjectRoot $ProjectRoot }

$dirty=[bool]$SourceDirty
if(-not $SourceRevision) {
    $src=Get-NativeSourceRevision -ProjectRoot $ProjectRoot
    $SourceRevision=$src.Revision
    $dirty=$src.Dirty
}
if(-not $Configuration) { $Configuration='Release' }

$metadata=[ordered]@{
    schema='xmb-build/1'
    sourceRevision=$SourceRevision
    sourceDirty=$dirty
    sourceFingerprint=$SourceFingerprint
    executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    builtAtUtc=[DateTime]::UtcNow.ToString('o')
    configuration=$Configuration
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'build-info.json') -Encoding UTF8
