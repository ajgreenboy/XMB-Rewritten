param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    # Optional overrides. build.ps1 passes its build-start snapshot so the recorded
    # fingerprint and revision are both from the build start; a direct CMake/Ninja
    # relink (CMakeLists.txt POST_BUILD) omits them and snapshots at link time.
    [string]$SourceRevision='',
    [string]$SourceFingerprint='',
    [switch]$SourceDirty,
    [string]$Configuration='',
    # Post-processing (e.g. after code signing rewrites the exe in place): keep the
    # recorded provenance untouched and only refresh the executable's hash.
    [switch]$RefreshExecutableHash
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'build-metadata.ps1')

$exe=Join-Path $OutputDirectory 're-ps3-native.exe'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "No re-ps3-native.exe at '$OutputDirectory'."
}

if($RefreshExecutableHash) {
    $infoPath=Join-Path $OutputDirectory 'build-info.json'
    if(Test-Path -LiteralPath $infoPath -PathType Leaf) {
        $info=Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
        $info.executableSha256=Get-NativeSha256 -LiteralPath $exe
        $info.builtAtUtc=[DateTime]::UtcNow.ToString('o')
        $info | ConvertTo-Json | Set-Content -LiteralPath $infoPath -Encoding UTF8
        return
    }
    # No existing metadata to refresh; fall through to a full write.
}

if(-not $SourceFingerprint) { $SourceFingerprint=Get-NativeSourceFingerprint -ProjectRoot $ProjectRoot }

$dirty=[bool]$SourceDirty
if(-not $SourceRevision) {
    $src=Get-NativeSourceRevision -ProjectRoot $ProjectRoot
    $SourceRevision=$src.Revision
    $dirty=$src.Dirty
}

$metadata=[ordered]@{
    schema='xmb-build/1'
    sourceRevision=$SourceRevision
    sourceDirty=$dirty
    sourceFingerprint=$SourceFingerprint
    executableSha256=(Get-NativeSha256 -LiteralPath $exe)
    builtAtUtc=[DateTime]::UtcNow.ToString('o')
    configuration=$Configuration
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'build-info.json') -Encoding UTF8
